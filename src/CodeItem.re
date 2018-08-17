open GenFlowCommon;

type importType =
  | ImportComment(string)
  | ImportAsFrom(string, option(string), ImportPath.t);

type exportType = {
  opaque: bool,
  typeVars: list(string),
  typeName: string,
  comment: option(string),
  typ,
};

type exportVariantType = {
  typeParams: list(typ),
  leafTypes: list(typ),
  name: string,
};

type componentBinding = {
  exportType,
  moduleName: ModuleName.t,
  propsTypeName: string,
  componentType: typ,
  typ,
};

type externalReactClass = {
  componentName: string,
  importPath: ImportPath.t,
};

type valueBinding = {
  moduleName: ModuleName.t,
  id: Ident.t,
  typ,
};

type t =
  | ImportType(importType)
  | ExternalReactClass(externalReactClass)
  | ValueBinding(valueBinding)
  | ConstructorBinding(
      exportType,
      typ,
      list(typ),
      string,
      Runtime.recordValue,
    )
  | ComponentBinding(componentBinding)
  | ExportType(exportType)
  | ExportVariantType(exportVariantType);

type genFlowKind =
  | NoGenFlow
  | GenFlow
  | GenFlowOpaque;

type translation = {
  dependencies: list(Dependencies.t),
  codeItems: list(t),
};

let combineTranslations = (translations: list(translation)): translation =>
  translations
  |> List.map(({dependencies, codeItems}) => (dependencies, codeItems))
  |> List.split
  |> (
    ((dependencies, codeItems)) => {
      dependencies: dependencies |> List.concat,
      codeItems: codeItems |> List.concat,
    }
  );

let getImportTypeName = importType =>
  switch (importType) {
  | ImportComment(s) => s
  | ImportAsFrom(s, _, _) => s
  };

let toString = (~language, codeItem) =>
  switch (codeItem) {
  | ImportType(importType) => "ImportType " ++ getImportTypeName(importType)
  | ExternalReactClass(externalReactClass) =>
    "ExternalReactClass " ++ externalReactClass.componentName
  | ValueBinding({moduleName, id, typ}) =>
    "ValueBinding"
    ++ " moduleName:"
    ++ ModuleName.toString(moduleName)
    ++ " id:"
    ++ Ident.name(id)
    ++ " typ:"
    ++ EmitTyp.typToString(~language, typ)
  | ConstructorBinding(_, _, _, variantName, _) =>
    "ConstructorBinding " ++ variantName
  | ComponentBinding(componentBinding) =>
    "ComponentBinding " ++ (componentBinding.moduleName |> ModuleName.toString)
  | ExportType(exportType) => "ExportType " ++ exportType.typeName
  | ExportVariantType(exportVariantType) =>
    "ExportVariantType " ++ exportVariantType.name
  };

let rec hasAttribute = (searchText, attributes) =>
  switch (attributes) {
  | [] => false
  | [({Asttypes.txt, _}, _), ..._tl] when txt == searchText => true
  | [_hd, ...tl] => hasAttribute(searchText, tl)
  };

let getGenFlowKind = attrs =>
  if (hasAttribute(tagSearch, attrs)) {
    GenFlow;
  } else if (hasAttribute(tagSearchOpaque, attrs)) {
    GenFlowOpaque;
  } else {
    NoGenFlow;
  };

let createFunctionType = (typeVars, argTypes, resultType) =>
  if (argTypes === []) {
    resultType;
  } else {
    Arrow(typeVars, argTypes, resultType);
  };

let exportType = (~opaque, ~typeVars, ~typeName, ~comment=?, typ) => {
  opaque,
  typeVars,
  typeName,
  comment,
  typ,
};

let translateExportType = (~opaque, ~typeVars, ~typeName, ~comment=?, typ) =>
  ExportType({opaque, typeVars, typeName, comment, typ});

let variantLeafTypeName = (typeName, leafName) =>
  String.capitalize(typeName) ++ String.capitalize(leafName);

/*
 * TODO: Make the types namespaced by nested Flow module.
 */
let translateConstructorDeclaration =
    (variantTypeName, constructorDeclaration, ~recordGen) => {
  let constructorArgs = constructorDeclaration.Types.cd_args;
  let variantName = Ident.name(constructorDeclaration.Types.cd_id);
  let argsTranslation = Dependencies.translateTypeExprs(constructorArgs);
  let argTypes = argsTranslation |> List.map(({Dependencies.typ, _}) => typ);
  let dependencies =
    argsTranslation
    |> List.map(({Dependencies.dependencies, _}) => dependencies)
    |> List.concat;
  /* A valid Reason identifier that we can point UpperCase JS exports to. */
  let variantTypeName = variantLeafTypeName(variantTypeName, variantName);

  let typeVars = argTypes |> TypeVars.freeOfList;

  let retType = Ident(variantTypeName, typeVars |> TypeVars.toTyp);
  let constructorTyp = createFunctionType(typeVars, argTypes, retType);
  let recordValue =
    recordGen |> Runtime.newRecordValue(~unboxed=constructorArgs == []);
  let codeItems = [
    ConstructorBinding(
      exportType(~opaque=true, ~typeVars, ~typeName=variantTypeName, any),
      constructorTyp,
      argTypes,
      variantName,
      recordValue,
    ),
  ];
  (retType, (dependencies, codeItems));
};

/* Applies type parameters to types (for all) */
let abstractTheTypeParameters = (~typeVars, typ) =>
  switch (typ) {
  | Optional(_)
  | Ident(_)
  | TypeVar(_)
  | ObjectType(_) => typ
  | Arrow(_, argTypes, retType) => Arrow(typeVars, argTypes, retType)
  };

let translateId = (~moduleName, ~valueBinding, id): translation => {
  let {Typedtree.vb_expr, _} = valueBinding;
  let typeExpr = vb_expr.exp_type;
  let typeExprTranslation = typeExpr |> Dependencies.translateTypeExpr;
  let typeVars = typeExprTranslation.typ |> TypeVars.free;
  let typ = typeExprTranslation.typ |> abstractTheTypeParameters(~typeVars);
  let codeItems = [ValueBinding({moduleName, id, typ})];
  {dependencies: typeExprTranslation.dependencies, codeItems};
};

/*
 * The `make` function is typically of the type:
 *
 *    (~named, ~args=?, 'childrenType) => ReasonReactComponentSpec<
 *      State,
 *      State,
 *      RetainedProps,
 *      RetainedProps,
 *      Action,
 *    >)
 *
 * We take a reference to that function and turn it into a React component of
 * type:
 *
 *
 *     exports.component = (component : React.Component<Props>);
 *
 * Where `Props` is of type:
 *
 *     {named: number, args?: number}
 */

let translateMake =
    (~language, ~propsTypeGen, ~moduleName, ~valueBinding, id): translation => {
  let {Typedtree.vb_expr, _} = valueBinding;
  let typeExpr = vb_expr.exp_type;
  let typeExprTranslation =
    typeExpr
    |> Dependencies.translateTypeExpr(
         /* Only get the dependencies for the prop types.
            The return type is a ReasonReact component. */
         ~noFunctionReturnDependencies=true,
       );

  let freeTypeVarsSet = typeExprTranslation.typ |> TypeVars.free_;

  /* Replace type variables in props/children with any. */
  let (typeVars, typ) = (
    [],
    typeExprTranslation.typ
    |> TypeVars.substitute(~f=s =>
         if (freeTypeVarsSet |> StringSet.mem(s)) {
           Some(any);
         } else {
           None;
         }
       ),
  );
  switch (typ) {
  | Arrow(
      _typeVars,
      [propOrChildren, ...childrenOrNil],
      Ident(
        "ReasonReactcomponentSpec" | "ReactcomponentSpec" |
        "ReasonReactcomponent" |
        "Reactcomponent",
        [_state, ..._],
      ),
    ) =>
    /* Add children?:any to props type */
    let propsTypeArguments =
      switch (childrenOrNil) {
      /* Then we only extracted a function that accepts children, no props */
      | [] => ObjectType([("children", NonMandatory, any)])
      /* Then we had both props and children. */
      | [children, ..._] =>
        switch (propOrChildren) {
        | ObjectType(fields) =>
          ObjectType(fields @ [("children", NonMandatory, children)])
        | _ => propOrChildren
        }
      };
    let propsTypeName = GenIdent.propsTypeName(~propsTypeGen);
    let componentType = EmitTyp.reactComponentType(~language, ~propsTypeName);

    let codeItems = [
      ComponentBinding({
        exportType:
          exportType(
            ~opaque=false,
            ~typeVars,
            ~typeName=propsTypeName,
            propsTypeArguments,
          ),
        moduleName,
        propsTypeName,
        componentType,
        typ,
      }),
    ];
    {dependencies: typeExprTranslation.dependencies, codeItems};

  | _ =>
    /* not a component: treat make as a normal function */
    id |> translateId(~moduleName, ~valueBinding)
  };
};

let translateValueBinding =
    (~language, ~propsTypeGen, ~moduleName, valueBinding): translation => {
  let {Typedtree.vb_pat, vb_attributes, _} = valueBinding;
  switch (vb_pat.pat_desc, getGenFlowKind(vb_attributes)) {
  | (Tpat_var(id, _), GenFlow) when Ident.name(id) == "make" =>
    id |> translateMake(~language, ~propsTypeGen, ~moduleName, ~valueBinding)
  | (Tpat_var(id, _), GenFlow) =>
    id |> translateId(~moduleName, ~valueBinding)
  | _ => {dependencies: [], codeItems: []}
  };
};

/**
 * [@genFlow]
 * [@bs.module] external myBanner : ReasonReact.reactClass = "./MyBanner";
 */
let translateValueDescription =
    (valueDescription: Typedtree.value_description): translation => {
  let componentName =
    valueDescription.val_id |> Ident.name |> String.capitalize;
  let path =
    switch (valueDescription.val_prim) {
    | [firstValPrim, ..._] => firstValPrim
    | [] => ""
    };
  let importPath = path |> ImportPath.fromStringUnsafe;
  let typeExprTranslation =
    valueDescription.val_desc.ctyp_type |> Dependencies.translateTypeExpr;
  let genFlowKind = getGenFlowKind(valueDescription.val_attributes);
  switch (typeExprTranslation.typ, genFlowKind) {
  | (Ident("ReasonReactreactClass", []), GenFlow) when path != "" => {
      dependencies: [],
      codeItems: [ExternalReactClass({componentName, importPath})],
    }
  | _ => {dependencies: [], codeItems: []}
  };
};

let hasSomeGADTLeaf = constructorDeclarations =>
  List.exists(
    declaration => declaration.Types.cd_res !== None,
    constructorDeclarations,
  );

let translateTypeDecl = (dec: Typedtree.type_declaration): translation =>
  switch (
    dec.typ_type.type_params,
    dec.typ_type.type_kind,
    getGenFlowKind(dec.typ_attributes),
  ) {
  | (typeParams, Type_record(_, _), GenFlow | GenFlowOpaque) =>
    let typeVars = TypeVars.extract(typeParams);
    let typeName = Ident.name(dec.typ_id);
    {
      dependencies: [],
      codeItems: [
        translateExportType(
          ~opaque=true,
          ~typeVars,
          ~typeName,
          ~comment="Record type not supported",
          any,
        ),
      ],
    };
  /*
   * This case includes aliasings such as:
   *
   *     type list('t) = List.t('t');
   */
  | (typeParams, Type_abstract, GenFlow | GenFlowOpaque)
  | (typeParams, Type_variant(_), GenFlowOpaque) =>
    let typeVars = TypeVars.extract(typeParams);
    let typeName = Ident.name(dec.typ_id);
    switch (dec.typ_manifest) {
    | None => {
        dependencies: [],
        codeItems: [
          translateExportType(~opaque=true, ~typeVars, ~typeName, any),
        ],
      }
    | Some(coreType) =>
      let opaque =
        switch (coreType.ctyp_desc) {
        | Ttyp_constr(
            Path.Pident({name: "int" | "bool" | "string" | "unit", _}),
            _,
            [],
          ) =>
          false
        | _ => true
        };
      let typeExprTranslation =
        coreType.Typedtree.ctyp_type |> Dependencies.translateTypeExpr;
      let codeItems = [
        translateExportType(
          ~opaque,
          ~typeVars,
          ~typeName,
          typeExprTranslation.typ,
        ),
      ];
      {dependencies: typeExprTranslation.dependencies, codeItems};
    };
  | (astTypeParams, Type_variant(constructorDeclarations), GenFlow)
      when !hasSomeGADTLeaf(constructorDeclarations) =>
    let variantTypeName = Ident.name(dec.typ_id);
    let resultTypesDepsAndVariantLeafBindings = {
      let recordGen = Runtime.recordGen();
      List.map(
        constructorDeclaration =>
          translateConstructorDeclaration(
            variantTypeName,
            constructorDeclaration,
            ~recordGen,
          ),
        constructorDeclarations,
      );
    };
    let (resultTypes, depsAndVariantLeafBindings) =
      List.split(resultTypesDepsAndVariantLeafBindings);
    let (listListDeps, listListItems) =
      List.split(depsAndVariantLeafBindings);
    let deps = List.concat(listListDeps);
    let items = List.concat(listListItems);
    let typeParams = TypeVars.(astTypeParams |> extract |> toTyp);
    let unionType =
      ExportVariantType({
        typeParams,
        leafTypes: resultTypes,
        name: variantTypeName,
      });
    {dependencies: deps, codeItems: List.append(items, [unionType])};
  | _ => {dependencies: [], codeItems: []}
  };

let typePathToImport =
    (
      ~config as {modulesMap, language},
      ~outputFileRelative,
      ~resolver,
      typePath,
    ) =>
  switch (typePath) {
  | Path.Pident(id) when Ident.name(id) == "list" =>
    ImportAsFrom(
      "list",
      None,
      ModuleName.reasonPervasives
      |> ModuleResolver.importPathForReasonModuleName(
           ~language,
           ~outputFileRelative,
           ~resolver,
           ~modulesMap,
         ),
    )

  | Path.Pident(id) =>
    ImportComment(
      "// No need to import locally visible type "
      ++ Ident.name(id)
      ++ ". Make sure it is also marked with @genFlow",
    )

  | Pdot(Papply(_, _), _, _)
  | Papply(_, _) => ImportComment("// Cannot import type with Papply")

  | Pdot(p, s, _pos) =>
    let moduleName =
      switch (p) {
      | Path.Pident(id) => id |> Ident.name |> ModuleName.fromStringUnsafe
      | Pdot(_, lastNameInPath, _) =>
        lastNameInPath |> ModuleName.fromStringUnsafe
      | Papply(_, _) => assert(false) /* impossible: handled above */
      };
    let typeName = s;
    ImportAsFrom(
      typeName,
      {
        let asTypeName = Dependencies.typePathToName(typePath);
        asTypeName == typeName ? None : Some(asTypeName);
      },
      moduleName
      |> ModuleResolver.importPathForReasonModuleName(
           ~language,
           ~outputFileRelative,
           ~resolver,
           ~modulesMap,
         ),
    );
  };

let importTypeCompare = (i1, i2) =>
  compare(i1 |> getImportTypeName, i2 |> getImportTypeName);

let translateDependencies =
    (~config, ~outputFileRelative, ~resolver, dependencies): list(t) => {
  let dependencyToImportType = dependency =>
    switch (dependency) {
    | Dependencies.TypeAtPath(p) =>
      typePathToImport(~config, ~outputFileRelative, ~resolver, p)
    };
  dependencies
  |> List.map(dependencyToImportType)
  |> List.sort_uniq(importTypeCompare)
  |> List.map(importType => ImportType(importType));
};