//===--- DerivedConformanceCodable.cpp - Derived Codable ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements explicit derivation of the Encodable and Decodable
// protocols for a struct or class.
//
//===----------------------------------------------------------------------===//
//
#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "DerivedConformances.h"

using namespace swift;
using namespace DerivedConformance;

/// Returns whether the type represented by the given ClassDecl inherits from a
/// type which conforms to the given protocol.
///
/// \param type The \c ClassDecl whose superclass to look up.
///
/// \param proto The protocol to check conformance for.
static bool inheritsConformanceTo(ClassDecl *type, ProtocolDecl *proto) {
  if (!type->hasSuperclass())
    return false;

  auto &C = type->getASTContext();
  auto *superclassDecl = type->getSuperclassDecl();
  auto *superclassModule = superclassDecl->getModuleContext();
  return (bool)superclassModule->lookupConformance(type->getSuperclass(),
                                                   proto,
                                                   C.getLazyResolver());
}

/// Returns whether the superclass of the given class conforms to Encodable.
///
/// \param type The \c ClassDecl whose superclass to check.
static bool superclassIsEncodable(ClassDecl *type) {
    auto &C = type->getASTContext();
    return inheritsConformanceTo(type,
                                 C.getProtocol(KnownProtocolKind::Encodable));
}

/// Returns whether the superclass of the given class conforms to Decodable.
///
/// \param type The \c ClassDecl whose superclass to check.
static bool superclassIsDecodable(ClassDecl *type) {
    auto &C = type->getASTContext();
    return inheritsConformanceTo(type,
                                 C.getProtocol(KnownProtocolKind::Decodable));
}

/// Validates that all the variables declared in the given list of declarations
/// conform to the given protocol.
///
/// Produces a diagnostic on the given typechecker for every var which does not
/// conform. Calls a success callback for every var which does conform.
///
/// \param tc The typechecker to use in validating {En,Decodable} conformance.
///
/// \param context The \c DeclContext the var declarations belong to.
///
/// \param vars The var range to validate.
///
/// \param proto The protocol to check conformance to.
///
/// \param callback A callback to call on every valid var decl.
template <typename ValidVarCallback>
static bool
validateVarsConformToProtocol(TypeChecker &tc, DeclContext *context,
                              NominalTypeDecl::StoredPropertyRange &vars,
                              ProtocolDecl *proto, ValidVarCallback &callback) {
  bool allConform = true;
  for (auto varDecl : vars) {
    // If the decl doesn't yet have a type, we may be seeing it before the type
    // checker has gotten around to evaluating its type. For example:
    //
    // func foo() {
    //   let b = Bar(from: decoder) // <- evaluates Bar conformance to Codable,
    //                              //    forcing derivation
    // }
    //
    // struct Bar : Codable {
    //   var x: Int // <- we get to valuate x's var decl here, but its type
    //              //    hasn't yet been evaluated
    // }
    //
    // Validate the decl eagerly.
    if (!varDecl->hasType())
      tc.validateDecl(varDecl);

    // If the var decl didn't validate, it may still not have a type; confirm it
    // has a type before ensuring the type conforms to Codable.
    if (!varDecl->hasType() ||
        !tc.conformsToProtocol(varDecl->getType(), proto, context,
                               ConformanceCheckFlags::Used)) {
      // TODO: We should produce a diagnostic note here explaining that we found
      //       a var not conforming to Codable.
      allConform = false;
      continue;
    }

    callback(varDecl);
  }

  return allConform;
}

/// Validates the given CodingKeys enum decl by ensuring its cases are a 1-to-1
/// match with the stored vars of the given type.
///
/// \param tc The typechecker to use in validating {En,Decodable} conformance.
///
/// \param codingKeysDecl The \c CodingKeys enum decl to validate.
///
/// \param type The nominal type decl to validate the \c CodingKeys against.
///
/// \param proto The {En,De}codable protocol to validate all the keys conform
/// to.
static bool validateCodingKeysEnum(TypeChecker &tc, EnumDecl *codingKeysDecl,
                                   NominalTypeDecl *type, ProtocolDecl *proto) {
  // Look through all var decls in the given type.
  // * Filter out lazy/computed vars (currently already done by
  //   getStoredProperties).
  // * Filter out ones which are present in the given decl (by name).
  //
  // If any of the entries in the CodingKeys decl are not present in the type
  // by name, then this decl doesn't match.
  // If there are any vars left in the type, then this decl doesn't match.
  //
  // NOTE: If we change the behavior to ignore vars with default values, then we
  //       can further filter out the type names to remove those which
  //       correspond to vars with default values.
  llvm::SmallDenseSet<Identifier, 8> names;

  auto storedProperties = type->getStoredProperties(/*skipInaccessible=*/true);
  auto validVarCallback = [&names](VarDecl *varDecl) {
    names.insert(varDecl->getName());
  };

  if (!validateVarsConformToProtocol(tc, type->getDeclContext(),
                                     storedProperties, proto, validVarCallback))
    return false;

  for (auto elt : codingKeysDecl->getAllElements()) {
    auto it = names.find(elt->getName());
    if (it == names.end()) {
      // TODO: Produce diagnostic here complaining that the CodingKeys enum
      //       contains a case which does not correspond to a var.
      // TODO: Investigate typo-correction here; perhaps the case name was
      //       misspelled and we can provide a fix-it.
      return false;
    }

    names.erase(it);
  }

  // TODO: Produce diagnostic here complaining that there are vars which are not
  //       listed in the CodingKeys enum.
  return names.empty();
}

/// Returns whether the given type has a valid nested \c CodingKeys enum.
///
/// If the type has an invalid \c CodingKeys entity, produces diagnostics to
/// complain about the error. In this case, the error result will be true -- in
/// the case where we don't have a valid CodingKeys enum and have produced
/// diagnostics here, we don't want to then attempt to synthesize a CodingKeys
/// enum.
///
/// \param tc The typechecker to use in validating {En,Decodable} conformance.
///
/// \param type The type decl whose nested \c CodingKeys type to validate.
///
/// \param proto The {En,De}codable protocol to ensure the properties matching
/// the keys conform to.
static std::pair</* has type? */ bool, /* error? */ bool>
hasValidCodingKeysEnum(TypeChecker &tc, NominalTypeDecl *type,
                       ProtocolDecl *proto) {
  auto &C = tc.Context;
  auto codingKeysDecls = type->lookupDirect(DeclName(C.Id_CodingKeys));
  if (codingKeysDecls.empty())
    return {/* has type? */ false, /* error? */ false};

  // Only ill-formed code would produce multiple results for this lookup.
  // This would get diagnosed later anyway, so we're free to only look at the
  // first result here.
  auto result = codingKeysDecls.front();

  auto *codingKeysTypeDecl = dyn_cast<TypeDecl>(result);
  if (!codingKeysTypeDecl) {
    // TODO: Produce a diagnostic complaining that the "CodingKeys" entity we
    //       found is not a type.
    return {/* has type? */ true, /* error? */ true};
  }

  // Ensure that the type we found conforms to the CodingKey protocol.
  auto *codingKeyProto = C.getProtocol(KnownProtocolKind::CodingKey);
  auto codingKeysType = codingKeysTypeDecl->getDeclaredInterfaceType();
  if (!tc.conformsToProtocol(codingKeysType, codingKeyProto,
                             type->getDeclContext(),
                             ConformanceCheckFlags::Used)) {
    // TODO: Produce a diagnostic complaining that the "CodingKeys" entity we
    //       found does not conform to CodingKey.
    return {/* has type? */ true, /* error? */ true};
  }

  // CodingKeys should eventually be an enum. If it's a typealias, we'll need to
  // follow it.
  auto *codingKeysEnum = dyn_cast<EnumDecl>(result);
  if (auto *typealias = dyn_cast<TypeAliasDecl>(result)) {
    // TODO: Do we have to follow through multiple layers of typealiases
    //       here? Or will getCanonicalType() do that for us?
    auto canType = codingKeysType->getCanonicalType();
    assert(canType);

    codingKeysEnum = dyn_cast<EnumDecl>(codingKeysType->getAnyNominal());
  }

  if (!codingKeysEnum) {
    // TODO: Produce a diagnostic complaining that we cannot derive Codable
    //       with a non-enum CodingKeys type.
    return {/* has type? */ true, /* error? */ true};
  }

  bool valid = validateCodingKeysEnum(tc, codingKeysEnum, type, proto);
  return {/* has type? */ true, /* error? */ !valid};
}

/// Synthesizes a new \c CodingKeys enum based on the {En,De}codable members of
/// the given type (\c nullptr if unable to synthesize).
///
/// If able to synthesize the enum, adds it directly to \c type.
///
/// \param tc The typechecker to use in validating {En,De}codable conformance.
///
/// \param type The nominal type decl whose nested \c CodingKeys type to
/// synthesize.
///
/// \param proto The {En,De}codable protocol to validate all the keys conform
/// to.
static EnumDecl *synthesizeCodingKeysEnum(TypeChecker &tc,
                                          NominalTypeDecl *type,
                                          ProtocolDecl *proto) {
  auto &C = tc.Context;
  auto *typeDC = cast<DeclContext>(type);

  // We want to look through all the var declarations of this type to create
  // enum cases based on those var names.
  auto *codingKeyProto = C.getProtocol(KnownProtocolKind::CodingKey);
  auto *codingKeyType = codingKeyProto->getDeclaredType();
  TypeLoc protoTypeLoc[1] = {TypeLoc::withoutLoc(codingKeyType)};
  MutableArrayRef<TypeLoc> inherited = C.AllocateCopy(protoTypeLoc);

  auto *enumDecl = new (C) EnumDecl(SourceLoc(), C.Id_CodingKeys, SourceLoc(),
                                    inherited, nullptr, typeDC);
  enumDecl->setImplicit();
  enumDecl->setAccessibility(Accessibility::Private);

  auto *enumDC = cast<DeclContext>(enumDecl);
  auto *mutableEnumDC = cast<IterableDeclContext>(enumDecl);

  // For classes which inherit from something Encodable or Decodable, we
  // provide case `super` as the first key (to be used in encoding super).
  auto *classDecl = dyn_cast<ClassDecl>(type);
  if (classDecl &&
      (superclassIsEncodable(classDecl) || superclassIsDecodable(classDecl))) {
    // TODO: Ensure the class doesn't already have or inherit a variable named
    // "`super`"; otherwise we will generate an invalid enum. In that case,
    // diagnose and bail.
    auto *super = new (C) EnumElementDecl(SourceLoc(), C.Id_super, TypeLoc(),
                                          /*HasArgumentType=*/false,
                                          SourceLoc(), nullptr, enumDC);
    super->setImplicit();
    mutableEnumDC->addMember(super);
  }

  // Each of these vars needs a case in the enum. For each var decl, if the type
  // conforms to {En,De}codable, add it to the enum.
  auto storedProperties = type->getStoredProperties(/*skipInaccessible=*/true);
  auto validVarCallback = [&C, &enumDC, &mutableEnumDC](VarDecl *varDecl) {
    auto *elt = new (C) EnumElementDecl(SourceLoc(), varDecl->getName(),
                                        TypeLoc(), /*HasArgumentType=*/false,
                                        SourceLoc(), nullptr, enumDC);
    elt->setImplicit();
    mutableEnumDC->addMember(elt);
  };

  if (!validateVarsConformToProtocol(tc, type->getDeclContext(),
                                     storedProperties, proto, validVarCallback))
    return nullptr;

  // Forcibly derive conformance to CodingKey.
  tc.checkConformancesInContext(enumDC, mutableEnumDC);

  // Add to the type.
  cast<IterableDeclContext>(type)->addMember(enumDecl);
  return enumDecl;
}

/// Creates a new var decl representing
///
///   var/let container : containerBase<keyType>
///
/// \c containerBase is the name of the type to use as the base (either
/// \c KeyedEncodingContainer or \c KeyedDecodingContainer).
///
/// \param C The AST context to create the decl in.
///
/// \param DC The \c DeclContext to create the decl in.
///
/// \param keyedContainerDecl The generic type to bind the key type in.
///
/// \param keyType The key type to bind to the container type.
///
/// \param isLet Whether to declare the variable as immutable.
static VarDecl *createKeyedContainer(ASTContext &C, DeclContext *DC,
                                     NominalTypeDecl *keyedContainerDecl,
                                     Type keyType, bool isLet) {
  // Bind Keyed*Container to Keyed*Container<KeyType>
  Type boundType[1] = {keyType};
  auto containerType = BoundGenericType::get(keyedContainerDecl, Type(),
                                             C.AllocateCopy(boundType));

  // let container : Keyed*Container<KeyType>
  auto *containerDecl = new (C) VarDecl(/*IsStatic=*/false, /*IsLet=*/isLet,
                                        /*IsCaptureList=*/false, SourceLoc(),
                                        C.Id_container, containerType, DC);
  containerDecl->setImplicit();
  containerDecl->setInterfaceType(containerType);
  return containerDecl;
}

/// Creates a new \c CallExpr representing
///
///   base.container(keyedBy: CodingKeys.self)
///
/// \param C The AST context to create the expression in.
///
/// \param DC The \c DeclContext to create any decls in.
///
/// \param base The base expression to make the call on.
///
/// \param returnType The return type of the call.
///
/// \param param The parameter to the call.
static CallExpr *createContainerKeyedByCall(ASTContext &C, DeclContext *DC,
                                            Expr *base, Type returnType,
                                            NominalTypeDecl *param) {
  // (keyedBy:)
  auto *keyedByDecl = new (C) ParamDecl(/*IsLet=*/true, SourceLoc(),
                                        SourceLoc(), C.Id_keyedBy, SourceLoc(),
                                        C.Id_keyedBy, returnType, DC);
  keyedByDecl->setImplicit();
  keyedByDecl->setInterfaceType(returnType);

  // container(keyedBy:) method name
  auto *paramList = ParameterList::createWithoutLoc(keyedByDecl);
  DeclName callName(C, C.Id_container, paramList);

  // base.container(keyedBy:) expr
  auto *unboundCall = new (C) UnresolvedDotExpr(base, SourceLoc(), callName,
                                                DeclNameLoc(),
                                                /*Implicit=*/true);

  // CodingKeys.self expr
  auto *codingKeysExpr = new (C) DeclRefExpr(ConcreteDeclRef(param),
                                             DeclNameLoc(), /*Implicit=*/true);
  auto *codingKeysMetaTypeExpr = new (C) DotSelfExpr(codingKeysExpr,
                                                     SourceLoc(), SourceLoc());

  // Full bound base.container(keyedBy: CodingKeys.self) call
  Expr *args[1] = {codingKeysMetaTypeExpr};
  Identifier argLabels[1] = {C.Id_keyedBy};
  return CallExpr::createImplicit(C, unboundCall, C.AllocateCopy(args),
                                  C.AllocateCopy(argLabels));
}

/// Synthesizes the body for `func encode(to encoder: Encoder) throws`.
///
/// \param encodeDecl The function decl whose body to synthesize.
static void deriveBodyEncodable_encode(AbstractFunctionDecl *encodeDecl) {
  // struct Foo : Codable {
  //   var x: Int
  //   var y: String
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case x
  //     case y
  //   }
  //
  //   @derived func encode(to encoder: Encoder) throws {
  //     var container = encoder.container(keyedBy: CodingKeys.self)
  //     try container.encode(x, forKey: .x)
  //     try container.encode(y, forKey: .y)
  //   }
  // }

  // The enclosing type decl.
  auto *typeDecl = cast<NominalTypeDecl>(encodeDecl->getDeclContext());

  auto *funcDC = cast<DeclContext>(encodeDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type.
  auto *codingKeysDecl = typeDecl->lookupDirect(DeclName(C.Id_CodingKeys))[0];
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  assert(codingKeysDecl && "Missing CodingKeys decl.");
  // b) The type is not an enum
  auto *codingKeysEnum = cast<EnumDecl>(codingKeysDecl);

  SmallVector<ASTNode, 5> statements;

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to encode super.

  // let container : KeyedEncodingContainer<CodingKeys>
  auto codingKeysType = codingKeysEnum->getDeclaredType();
  auto *containerDecl = createKeyedContainer(C, funcDC,
                                             C.getKeyedEncodingContainerDecl(),
                                             codingKeysType, /*isLet=*/false);

  auto *containerExpr = new (C) DeclRefExpr(ConcreteDeclRef(containerDecl),
                                            DeclNameLoc(), /*Implicit=*/true,
                                            AccessSemantics::DirectToStorage);

  // Need to generate
  //   `let container = encoder.container(keyedBy: CodingKeys.self)`
  // This is unconditional because a type with no properties should encode as an
  // empty container.
  //
  // `let container` (containerExpr) is generated above.

  // encoder
  auto encoderParam = encodeDecl->getParameterList(1)->get(0);
  auto *encoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(encoderParam),
                                          DeclNameLoc(), /*Implicit=*/true);

  // Bound encoder.container(keyedBy: CodingKeys.self) call
  auto containerType = containerDecl->getInterfaceType();
  auto *callExpr = createContainerKeyedByCall(C, funcDC, encoderExpr,
                                              containerType, codingKeysEnum);

  // Full `let container = encoder.container(keyedBy: CodingKeys.self)`
  // binding.
  auto *containerPattern = new (C) NamedPattern(containerDecl,
                                                /*implicit=*/true);
  auto *bindingDecl = PatternBindingDecl::create(C, SourceLoc(),
                                                 StaticSpellingKind::None,
                                                 SourceLoc(),
                                                 containerPattern, callExpr,
                                                 funcDC);
  statements.push_back(bindingDecl);
  statements.push_back(containerDecl);

  // Now need to generate `try container.encode(x, forKey: .x)` for all
  // existing properties.
  for (auto *elt : codingKeysEnum->getAllElements()) {
    // Only ill-formed code would produce multiple results for this lookup.
    // This would get diagnosed later anyway, so we're free to only look at
    // the first result here.
    auto matchingVars = typeDecl->lookupDirect(DeclName(elt->getName()));

    // self.x
    auto *selfRef = createSelfDeclRef(encodeDecl);
    auto *varExpr = new (C) MemberRefExpr(selfRef, SourceLoc(),
                                          ConcreteDeclRef(matchingVars[0]),
                                          DeclNameLoc(), /*Implicit=*/true);

    // CodingKeys.x
    auto *eltRef = new (C) DeclRefExpr(elt, DeclNameLoc(), /*implicit=*/true);
    auto *metaTyRef = TypeExpr::createImplicit(codingKeysType, C);
    auto *keyExpr = new (C) DotSyntaxCallExpr(eltRef, SourceLoc(), metaTyRef);

    // encode(_:forKey:)
    SmallVector<Identifier, 2> argNames{Identifier(), C.Id_forKey};
    DeclName name(C, C.Id_encode, argNames);
    auto *encodeCall = new (C) UnresolvedDotExpr(containerExpr, SourceLoc(),
                                                 name, DeclNameLoc(),
                                                 /*Implicit=*/true);

    // container.encode(self.x, forKey: CodingKeys.x)
    Expr *args[2] = {varExpr, keyExpr};
    auto *callExpr = CallExpr::createImplicit(C, encodeCall,
                                              C.AllocateCopy(args),
                                              C.AllocateCopy(argNames));

    // try container.encode(self.x, forKey: CodingKeys.x)
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*Implicit=*/true);
    statements.push_back(tryExpr);
  }

  // Classes which inherit from something Codable should encode super as well.
  auto *classDecl = dyn_cast<ClassDecl>(typeDecl);
  if (classDecl && superclassIsEncodable(classDecl)) {
    // Need to generate `try super.encode(to: container.superEncoder())`

    // superEncoder()
    auto *method = new (C) UnresolvedDeclRefExpr(
        DeclName(C.Id_superEncoder), DeclRefKind::Ordinary, DeclNameLoc());

    // container.superEncoder()
    auto *superEncoderRef = new (C) DotSyntaxCallExpr(containerExpr,
                                                      SourceLoc(), method);

    // encode(to:) expr
    auto *encodeDeclRef = new (C) DeclRefExpr(ConcreteDeclRef(encodeDecl),
                                              DeclNameLoc(), /*Implicit=*/true);

    // super
    auto *superRef = new (C) SuperRefExpr(encodeDecl->getImplicitSelfDecl(),
                                          SourceLoc(), /*Implicit=*/true);

    // super.encode(to:)
    auto *encodeCall = new (C) DotSyntaxCallExpr(superRef, SourceLoc(),
                                                 encodeDeclRef);

    // super.encode(to: container.superEncoder())
    Expr *args[1] = {superEncoderRef};
    Identifier argLabels[1] = {C.Id_to};
    auto *callExpr = CallExpr::createImplicit(C, encodeCall,
                                              C.AllocateCopy(args),
                                              C.AllocateCopy(argLabels));

    // try super.encode(to: container.superEncoder())
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*Implicit=*/true);
    statements.push_back(tryExpr);
  }

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  encodeDecl->setBody(body);
}

/// Synthesizes a function declaration for `encode(to: Encoder) throws` with a
/// lazily synthesized body for the given type.
///
/// Adds the function declaration to the given type before returning it.
///
/// \param tc The type checker whose AST context to synthesize the decl in.
///
/// \param parentDecl The parent declaration of the type.
///
/// \param type The nominal type to synthesize the function for.
static FuncDecl *deriveEncodable_encode(TypeChecker &tc, Decl *parentDecl,
                                        NominalTypeDecl *type) {
  auto &C = tc.Context;
  auto *typeDC = cast<DeclContext>(type);

  // Expected type: (Self) -> (Encoder) throws -> ()
  // Constructed as: func type
  //                 input: Self
  //                 throws
  //                 output: function type
  //                         input: Encoder
  //                         output: ()
  // Create from the inside out:

  // (to: Encoder)
  auto encoderType = C.getEncoderDecl()->getDeclaredInterfaceType();
  auto inputTypeElt = TupleTypeElt(encoderType, C.Id_to);
  auto inputType = TupleType::get(ArrayRef<TupleTypeElt>(inputTypeElt), C);

  // throws
  auto extInfo = FunctionType::ExtInfo(FunctionTypeRepresentation::Swift,
                                       /*Throws=*/true);
  // ()
  auto returnType = TupleType::getEmpty(C);

  // (to: Encoder) throws -> ()
  auto innerType = FunctionType::get(inputType, returnType, extInfo);

  // Params: (self [implicit], Encoder)
  auto *selfDecl = ParamDecl::createSelf(SourceLoc(), typeDC);
  auto *encoderParam = new (C) ParamDecl(/*isLet=*/true, SourceLoc(),
                                         SourceLoc(), C.Id_to, SourceLoc(),
                                         C.Id_encoder, encoderType, typeDC);
  encoderParam->setInterfaceType(encoderType);

  ParameterList *params[] = {ParameterList::createWithoutLoc(selfDecl),
                             ParameterList::createWithoutLoc(encoderParam)};

  // Func name: encode(to: Encoder)
  DeclName name(C, C.Id_encode, params[1]);
  auto *encodeDecl = FuncDecl::create(C, SourceLoc(), StaticSpellingKind::None,
                                      SourceLoc(), name, SourceLoc(),
                                      /*Throws=*/true, SourceLoc(), SourceLoc(),
                                      nullptr, params,
                                      TypeLoc::withoutLoc(returnType), typeDC);
  encodeDecl->setImplicit();
  encodeDecl->setBodySynthesizer(deriveBodyEncodable_encode);

  // This method should be marked as 'override' for classes inheriting Encodable
  // conformance from a parent class.
  auto *classDecl = dyn_cast<ClassDecl>(type);
  if (classDecl && superclassIsEncodable(classDecl)) {
    auto *attr = new (C) SimpleDeclAttr<DAK_Override>(/*IsImplicit=*/true);
    encodeDecl->getAttrs().add(attr);
  }

  // Evaluate the type of Self in (Self) -> (Encoder) throws -> ().
  Type selfType = typeDC->getDeclaredInterfaceType();
  Type interfaceType;
  if (auto sig = typeDC->getGenericSignatureOfContext()) {
    // Evaluate the below, but in a generic environment (if Self is generic).
    encodeDecl->setGenericEnvironment(typeDC->getGenericEnvironmentOfContext());
    interfaceType = GenericFunctionType::get(sig, selfType, innerType,
                                             FunctionType::ExtInfo());
  } else {
    // (Self) -> innerType == (Encoder) throws -> ()
    interfaceType = FunctionType::get(selfType, innerType);
  }

  encodeDecl->setInterfaceType(interfaceType);
  encodeDecl->setAccessibility(std::max(type->getFormalAccess(),
                                        Accessibility::Internal));

  // If the type was not imported, the derived conformance is either from the
  // type itself or an extension, in which case we will emit the declaration
  // normally.
  if (type->hasClangNode())
    tc.Context.addExternalDecl(encodeDecl);

  cast<IterableDeclContext>(type)->addMember(encodeDecl);
  return encodeDecl;
}

/// Synthesizes the body for `init(from decoder: Decoder) throws`.
///
/// \param initDecl The function decl whose body to synthesize.
static void deriveBodyDecodable_init(AbstractFunctionDecl *initDecl) {
  // struct Foo : Codable {
  //   var x: Int
  //   var y: String
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case x
  //     case y
  //   }
  //
  //   @derived init(from decoder: Decoder) throws {
  //     let container = try decoder.container(keyedBy: CodingKeys.self)
  //     x = try container.decode(Type.self, forKey: .x)
  //     y = try container.decode(Type.self, forKey: .y)
  //   }
  // }

  // The enclosing type decl.
  auto *typeDecl = cast<NominalTypeDecl>(initDecl->getDeclContext());

  auto *funcDC = cast<DeclContext>(initDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type.
  auto *codingKeysDecl = typeDecl->lookupDirect(DeclName(C.Id_CodingKeys))[0];
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  assert(codingKeysDecl && "Missing CodingKeys decl.");
  // b) The type is not an enum
  auto *codingKeysEnum = cast<EnumDecl>(codingKeysDecl);

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to decode super.

  // let container : KeyedDecodingContainer<CodingKeys>
  auto codingKeysType = codingKeysEnum->getDeclaredType();
  auto *containerDecl = createKeyedContainer(C, funcDC,
                                             C.getKeyedDecodingContainerDecl(),
                                             codingKeysType, /*isLet=*/true);

  auto *containerExpr = new (C) DeclRefExpr(ConcreteDeclRef(containerDecl),
                                            DeclNameLoc(), /*Implicit=*/true,
                                            AccessSemantics::DirectToStorage);

  SmallVector<ASTNode, 5> statements;
  auto enumElements = codingKeysEnum->getAllElements();
  if (!enumElements.empty()) {
    // Need to generate
    //   `let container = try decoder.container(keyedBy: CodingKeys.self)`
    // `let container` (containerExpr) is generated above.

    // decoder
    auto decoderParam = initDecl->getParameterList(1)->get(0);
    auto *decoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(decoderParam),
                                            DeclNameLoc(), /*Implicit=*/true);

    // Bound decoder.container(keyedBy: CodingKeys.self) call
    auto containerType = containerDecl->getInterfaceType();
    auto *callExpr = createContainerKeyedByCall(C, funcDC, decoderExpr,
                                                containerType, codingKeysEnum);

    // try decoder.container(keyedBy: CodingKeys.self)
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*implicit=*/true);

    // Full `let container = decoder.container(keyedBy: CodingKeys.self)`
    // binding.
    auto *containerPattern = new (C) NamedPattern(containerDecl,
                                                  /*implicit=*/true);
    auto *bindingDecl = PatternBindingDecl::create(C, SourceLoc(),
                                                   StaticSpellingKind::None,
                                                   SourceLoc(),
                                                   containerPattern, tryExpr,
                                                   funcDC);
    statements.push_back(bindingDecl);
    statements.push_back(containerDecl);

    // Now need to generate `x = try container.encode(Type.self, forKey: .x)`
    // for all existing properties.
    for (auto *elt : enumElements) {
      // Only ill-formed code would produce multiple results for this lookup.
      // This would get diagnosed later anyway, so we're free to only look at
      // the first result here.
      auto matchingVars = typeDecl->lookupDirect(DeclName(elt->getName()));
      auto *varDecl = cast<VarDecl>(matchingVars[0]);

      // Don't output a decode statement for a var let with a default value.
      if (varDecl->isLet() && varDecl->getParentInitializer() != nullptr)
        continue;

      // Type.self (where Type === type(of: x)
      auto varType = varDecl->getType();
      auto *metaTyRef = TypeExpr::createImplicit(varType, C);
      auto *typeExpr = new (C) DotSelfExpr(metaTyRef, SourceLoc(), SourceLoc(),
                                           varType);

      // CodingKeys.x
      auto *eltRef = new (C) DeclRefExpr(elt, DeclNameLoc(), /*implicit=*/true);
      metaTyRef = TypeExpr::createImplicit(codingKeysType, C);
      auto *keyExpr = new (C) DotSyntaxCallExpr(eltRef, SourceLoc(), metaTyRef);

      // container.decode(_:forKey:)
      SmallVector<Identifier, 2> argNames{Identifier(), C.Id_forKey};
      DeclName name(C, C.Id_decode, argNames);
      auto *decodeCall = new (C) UnresolvedDotExpr(containerExpr, SourceLoc(),
                                                   name, DeclNameLoc(),
                                                   /*Implicit=*/true);

      // container.decode(Type.self, forKey: CodingKeys.x)
      Expr *args[2] = {typeExpr, keyExpr};
      auto *callExpr = CallExpr::createImplicit(C, decodeCall,
                                                C.AllocateCopy(args),
                                                C.AllocateCopy(argNames));

      // try container.decode(Type.self, forKey: CodingKeys.x)
      auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                      /*Implicit=*/true);

      auto *selfRef = createSelfDeclRef(initDecl);
      auto *varExpr = new (C) UnresolvedDotExpr(
          selfRef, SourceLoc(), DeclName(varDecl->getName()), DeclNameLoc(),
          /*implicit=*/true);
      auto *assignExpr = new (C) AssignExpr(varExpr, SourceLoc(), tryExpr,
                                            /*Implicit=*/true);
      statements.push_back(assignExpr);
    }
  }

  // Classes which inherit from something Decodable should decode super as well.
  auto *classDecl = dyn_cast<ClassDecl>(typeDecl);
  if (classDecl && superclassIsDecodable(classDecl)) {
    // Need to generate `try super.init(from: container.superDecoder())`

    // superDecoder()
    auto *method = new (C) UnresolvedDeclRefExpr(
        DeclName(C.Id_superDecoder), DeclRefKind::Ordinary, DeclNameLoc());

    // container.superDecoder()
    auto *superDecoderRef = new (C) DotSyntaxCallExpr(containerExpr,
                                                      SourceLoc(), method);

    // init(from:) expr
    auto *initDeclRef = new (C) DeclRefExpr(ConcreteDeclRef(initDecl),
                                            DeclNameLoc(), /*Implicit=*/true);

    // super
    auto *superRef = new (C) SuperRefExpr(initDecl->getImplicitSelfDecl(),
                                          SourceLoc(), /*Implicit=*/true);

    // super.init(from:)
    auto *decodeCall = new (C) DotSyntaxCallExpr(superRef, SourceLoc(),
                                                 initDeclRef);

    // super.decode(from: container.superDecoder())
    Expr *args[1] = {superDecoderRef};
    Identifier argLabels[1] = {C.Id_from};
    auto *callExpr = CallExpr::createImplicit(C, decodeCall,
                                              C.AllocateCopy(args),
                                              C.AllocateCopy(argLabels));

    // try super.init(from: container.superDecoder())
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*Implicit=*/true);
    statements.push_back(tryExpr);
  }

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  initDecl->setBody(body);
}

/// Synthesizes a function declaration for `init(from: Decoder) throws` with a
/// lazily synthesized body for the given type.
///
/// Adds the function declaration to the given type before returning it.
///
/// \param tc The type checker whose AST context to synthesize the decl in.
///
/// \param parentDecl The parent declaration of the type.
///
/// \param type The nominal type to synthesize the function for.
static ValueDecl *deriveDecodable_init(TypeChecker &tc, Decl *parentDecl,
                                       NominalTypeDecl *type) {
  auto &C = tc.Context;
  auto *typeDC = cast<DeclContext>(type);

  // Expected type: (Self) -> (Decoder) throws -> (Self)
  // Constructed as: func type
  //                 input: Self
  //                 throws
  //                 output: function type
  //                         input: Encoder
  //                         output: Self
  // Compute from the inside out:

  // (from: Decoder)
  auto decoderType = C.getDecoderDecl()->getDeclaredInterfaceType();
  auto inputTypeElt = TupleTypeElt(decoderType, C.Id_from);
  auto inputType = TupleType::get(ArrayRef<TupleTypeElt>(inputTypeElt), C);

  // throws
  auto extInfo = FunctionType::ExtInfo(FunctionTypeRepresentation::Swift,
                                       /*Throws=*/true);

  // (Self)
  auto returnType = typeDC->getDeclaredInterfaceType();

  // (from: Decoder) throws -> (Self)
  Type innerType = FunctionType::get(inputType, returnType, extInfo);

  // Params: (self [implicit], Decoder)
  // self should be inout if the type is a value type; not inout otherwise.
  auto inOut = !isa<ClassDecl>(type);
  auto *selfDecl = ParamDecl::createSelf(SourceLoc(), typeDC,
                                         /*isStatic=*/false,
                                         /*isInOut=*/inOut);
  auto *decoderParamDecl = new (C) ParamDecl(/*isLet=*/true, SourceLoc(),
                                             SourceLoc(), C.Id_from,
                                             SourceLoc(), C.Id_decoder,
                                             decoderType, typeDC);
  decoderParamDecl->setImplicit();
  decoderParamDecl->setInterfaceType(decoderType);

  auto *paramList = ParameterList::createWithoutLoc(decoderParamDecl);

  // Func name: init(from: Decoder)
  DeclName name(C, C.Id_init, paramList);

  auto *initDecl = new (C) ConstructorDecl(
      name, SourceLoc(),
      /*Failability=*/OTK_None,
      /*FailabilityLoc=*/SourceLoc(),
      /*Throws=*/true, /*ThrowsLoc=*/SourceLoc(), selfDecl, paramList,
      /*GenericParams=*/nullptr, typeDC);
  initDecl->setImplicit();
  initDecl->setBodySynthesizer(deriveBodyDecodable_init);

  // This constructor should be marked as `required` for non-final classes.
  if (isa<ClassDecl>(type) && !type->getAttrs().hasAttribute<FinalAttr>()) {
    auto *reqAttr = new (C) SimpleDeclAttr<DAK_Required>(/*IsImplicit=*/true);
    initDecl->getAttrs().add(reqAttr);
  }

  Type selfType = initDecl->computeInterfaceSelfType();
  Type selfInitType = initDecl->computeInterfaceSelfType(/*init=*/true);
  Type interfaceType;
  Type initializerType;
  if (auto sig = typeDC->getGenericSignatureOfContext()) {
    // Evaluate the below, but in a generic environment (if Self is generic).
    initDecl->setGenericEnvironment(typeDC->getGenericEnvironmentOfContext());
    interfaceType = GenericFunctionType::get(sig, selfType, innerType,
                                             FunctionType::ExtInfo());
    initializerType = GenericFunctionType::get(sig, selfInitType, innerType,
                                               FunctionType::ExtInfo());
  } else {
    // (Self) -> (Decoder) throws -> (Self)
    interfaceType = FunctionType::get(selfType, innerType);
    initializerType = FunctionType::get(selfInitType, innerType);
  }

  initDecl->setInterfaceType(interfaceType);
  initDecl->setInitializerInterfaceType(initializerType);
  initDecl->setAccessibility(
      std::max(type->getFormalAccess(), Accessibility::Internal));

  // If the type was not imported, the derived conformance is either from the
  // type itself or an extension, in which case we will emit the declaration
  // normally.
  if (type->hasClangNode())
    tc.Context.addExternalDecl(initDecl);

  cast<IterableDeclContext>(type)->addMember(initDecl);
  return initDecl;
}

/// Returns whether the given type is valid for synthesizing {En,De}codable.
///
/// Checks to see whether the given type has a valid \c CodingKeys enum, and if
/// not, attempts to synthesize one for it.
///
/// \param tc The typechecker to use in validating {En,Decodable} conformance.
///
/// \param type The type to validate.
///
/// \param proto The *codable protocol to check for validity.
static bool canSynthesize(TypeChecker &tc, NominalTypeDecl *type,
                          ProtocolDecl *proto) {
  // First, look up if the type has a valid CodingKeys enum we can use.
  bool hasType, error;
  std::tie(hasType, error) = hasValidCodingKeysEnum(tc, type, proto);

  // We found a type, but it wasn't valid.
  if (error)
    return false;

  // We can try to synthesize a type here.
  if (!hasType) {
    auto *synthesizedEnum = synthesizeCodingKeysEnum(tc, type, proto);
    if (!synthesizedEnum)
      return false;
  }

  return true;
}

ValueDecl *DerivedConformance::deriveEncodable(TypeChecker &tc,
                                              Decl *parentDecl,
                                              NominalTypeDecl *type,
                                              ValueDecl *requirement) {
    // We can only synthesize Encodable for structs and classes.
    if (!isa<StructDecl>(type) && !isa<ClassDecl>(type))
        return nullptr;

    if (requirement->getName() != tc.Context.Id_encode) {
        // Unknown requirement.
        tc.diagnose(requirement->getLoc(), diag::broken_encodable_requirement);
        return nullptr;
    }

    // Check other preconditions for synthesized conformance.
    // This synthesizes a CodingKeys enum if possible.
    auto encodableProto = tc.Context.getProtocol(KnownProtocolKind::Encodable);
    if (canSynthesize(tc, type, encodableProto))
        return deriveEncodable_encode(tc, parentDecl, type);

    // Known protocol requirement but could not synthesize.
    // FIXME: We have to output at least one error diagnostic here because we
    // returned true from NominalTypeDecl::derivesProtocolConformance; if we
    // don't, we expect to return a witness here later and crash on an
    // assertion.  Producing an error stops compilation before then.
    auto encodableType = encodableProto->getDeclaredType();
    tc.diagnose(type, diag::type_does_not_conform, type->getDeclaredType(),
                encodableType);
    tc.diagnose(requirement, diag::no_witnesses, diag::RequirementKind::Func,
                requirement->getFullName(), encodableType, /*AddFixIt=*/false);
    return nullptr;
}

ValueDecl *DerivedConformance::deriveDecodable(TypeChecker &tc,
                                               Decl *parentDecl,
                                               NominalTypeDecl *type,
                                               ValueDecl *requirement) {
    // We can only synthesize Encodable for structs and classes.
    if (!isa<StructDecl>(type) && !isa<ClassDecl>(type))
        return nullptr;

    if (requirement->getName() != tc.Context.Id_init) {
        // Unknown requirement.
        tc.diagnose(requirement->getLoc(), diag::broken_decodable_requirement);
        return nullptr;
    }

    // Check other preconditions for synthesized conformance.
    // This synthesizes a CodingKeys enum if possible.
    auto decodableProto = tc.Context.getProtocol(KnownProtocolKind::Decodable);
    if (canSynthesize(tc, type, decodableProto))
        return deriveDecodable_init(tc, parentDecl, type);

    // Known protocol requirement but could not synthesize.
    // FIXME: We have to output at least one error diagnostic here because we
    // returned true from NominalTypeDecl::derivesProtocolConformance; if we
    // don't, we expect to return a witness here later and crash on an
    // assertion.  Producing an error stops compilation before then.
    auto decodableType = decodableProto->getDeclaredType();
    tc.diagnose(type, diag::type_does_not_conform, type->getDeclaredType(),
                decodableType);
    tc.diagnose(requirement, diag::no_witnesses,
                diag::RequirementKind::Constructor, requirement->getFullName(),
                decodableType, /*AddFixIt=*/false);
    return nullptr;
}
