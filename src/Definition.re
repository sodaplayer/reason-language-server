/*

 Ok folks, what I think I want is ...
 to compute for the whole file and then cache that.
 Also that way I can better handle definitions

 What will come out of this?
 A mapping of stamp -> (location, type, option(docs))
 And toplevelname -> stamp
 andddd maybe that's it?
 Oh right, a list of [loc, type, path] for the hover bit
 and probably a happing of stamp -> list(loc) of references

 umm I also want open mapping

 also thinking about providing rename functionality, and "find references"


 err what about stamps that are modules?
 maybe have a separate map for that?

 */
type item =
  | Module(list((string, int)))
  /* | ModuleAlias(Path.t) */
  | Type(Types.type_declaration)
  | Value(Types.type_expr);

type definition =
  | Path(Path.t)
  /* | Location(Location.t) */
  | Constructor(Path.t, string, Location.t)
  | Attribute(Path.t, string, Location.t)
  | IsDefinition;

type moduleData = {
  stamps: Hashtbl.t(int, (string, Location.t, item, option(string), ((int, int), (int, int)))),
  /* TODO track constructor names, and record attribute names */
  /* references: Hashtbl.t(int, list(Location.t)), */
  exported: Hashtbl.t(string, int),
  mutable topLevel: list((string, int)),
  mutable locations: list((Location.t, Types.type_expr, definition))
};

let rec docsItem = (item, data) => switch item {
| Type(t) => Docs.Type(t)
| Value(t) => Docs.Value(t)
| Module(items) => Docs.Module(items |> List.map(((name, stamp)) => {
  let (name, loc, item, docs, _) = Hashtbl.find(data.stamps, stamp);
  (name, loc, docs, docsItem(item, data))
}))
};

/* TODO this is not perfect, because if the user edits and gets outside of the original scope, then
we no longer give you the completions you need. This is annoying :/
Not sure how annoying in practice? One hack would be to forgive going a few lines over... */
let completions = ({stamps}, prefix, (l, c)) => {
  let l = l + 1;
  Hashtbl.fold((_, (name, loc, item, docs, ((l0, c0), (l1, c1))), results) => {
    if ((l0 < l || (l0 == l && c0 <= c)) &&
    ((l1 == -1 && c1 == -1) ||
    (l1 > l || (l1 == l && c1 > c))
    ) && Utils.startsWith(name, prefix)) {
      [(name, loc, item, docs), ...results]
    } else {
      results
    }
  }, stamps, [])
};

let listExported = data => {
  Hashtbl.fold((name, stamp, results) => {
    switch (Hashtbl.find(data.stamps, stamp)) {
    | exception Not_found => results
    | item => [item, ...results]
    };
  }, data.exported, [])
};

let listTopLevel = data => {
  data.topLevel |> List.map(((name, stamp)) => Hashtbl.find(data.stamps, stamp));
};

let resolvePath = (data, path) => {
  switch (path) {
  | [] => None
  | [one, ...rest] =>
    switch (Hashtbl.find(data.exported, one)) {
    | exception Not_found => None
    | stamp =>
      let rec loop = (stamp, path) => {
        switch (Hashtbl.find(data.stamps, stamp)) {
          | exception Not_found => None
          | (name, loc, item, docs, scope) => {
            switch (path, item) {
            | ([], _) => Some((name, loc, item, docs))
            | ([first, ...rest], Module(contents)) => switch (List.assoc(first, contents)) {
              | exception Not_found => None
              | stamp => loop(stamp, rest)
              }
            | _ => None
            }
          }
        }
      };
      loop(stamp, rest)
    };
  }
};

module Get = {
  /* TODO maybe return loc from this? or have a separate one that
   * finds a thing by name...
   */
  let rec stampNames = items =>
    Typedtree.(
      items
      |> List.map(item =>
           switch item.str_desc {
           | Tstr_value(_, bindings) =>
             bindings
             |> PrepareUtils.filterNil(binding =>
                  switch binding {
                  | {vb_pat: {pat_desc: Tpat_var({stamp, name}, _)}} =>
                    Some((name, stamp))
                  | _ => None
                  }
                )
           | Tstr_type(decls) =>
             decls |> List.map(({typ_id: {stamp, name}}) => (name, stamp))
           | Tstr_module({mb_id: {stamp, name}}) => [(name, stamp)]
           | Tstr_modtype({mtd_id: {stamp, name}}) => [(name, stamp)]
           /* | Tstr_include({incl_type}) */
           | _ => []
           }
         )
      |> List.concat
    );
  module F = (Collector: {let data: moduleData;}) => {
    open Typedtree;
    include TypedtreeIter.DefaultIteratorArgument;

    let posOfLexing = ({Lexing.pos_lnum, pos_cnum, pos_bol}) => (pos_lnum, pos_cnum - pos_bol);

    let rangeOfLoc = ({Location.loc_start, loc_end}) => (
      posOfLexing(loc_start),
      posOfLexing(loc_end)
    );

    let scopes = ref([((0, 0), (-1, -1))]);
    let addScope = loc => scopes := [loc, ...scopes^];
    let popScope = () => scopes := switch (scopes^) {
    | [] => []
    | [_, ...rest] => rest
    };
    let currentScope = () => List.hd(scopes^);

    let addStamp = (stamp, name, loc, item, docs) =>
      if (! Hashtbl.mem(Collector.data.stamps, stamp)) {
        Hashtbl.replace(Collector.data.stamps, stamp, (name, loc, item, docs, currentScope()));
      };

    let addLocation = (loc, typ, definition) =>
      Collector.data.locations = [
        (loc, typ, definition),
        ...Collector.data.locations
      ];

    let enter_signature_item = item => switch item.sig_desc {
      | Tsig_value({val_id: {stamp, name}, val_val: {val_type}, val_loc}) => addStamp(stamp, name, val_loc, Value(val_type), None)
      | Tsig_type(decls) => List.iter(({typ_id: {stamp, name}, typ_loc, typ_type}) => addStamp(stamp, name, typ_loc, Type(typ_type), None), decls)
      /* TODO add support for these */
      /* | Tsig_include({incl_mod, incl_type}) => stampsFromTypesSignature(currentPath, incl_type) */
      /* | Tsig_module({md_id: {stamp, name}, md_type: {mty_desc: Tmty_signature(signature)}}) => {
        addStamp
        let (stamps) = stampsFromTypedtreeInterface(addToPath(currentPath, name), signature.sig_items);
        [(stamp, addToPath(currentPath, name) |> toFullPath(PModule)), ...stamps]
      } */
      | Tsig_module({md_id: {stamp, name}, md_loc}) => addStamp(stamp, name, md_loc, Module([]), None)
      | _ => ()
    };

    let enter_structure_item = item =>
      Typedtree.(
        switch item.str_desc {
        | Tstr_value(_rec, bindings) =>
          /* TODO limit toplevel value completions */
          bindings
          |> List.iter(binding =>
               switch binding {
               | {
                   vb_attributes,
                   vb_pat: {pat_type, pat_desc: Tpat_var({stamp, name}, {loc})}
                 } =>
                 let docs = PrepareUtils.findDocAttribute(vb_attributes);
                 addStamp(stamp, name, loc, Value(pat_type), docs);
               /* addLocation(loc, pat_type, None); */
               | _ => ()
               }
             )
        | Tstr_type(decls) =>
          decls
          |> List.iter(
               (
                 {
                   typ_attributes,
                   typ_id: {stamp, name},
                   typ_type,
                   typ_name: {loc}
                 }
               )
               => {
                 let docs = PrepareUtils.findDocAttribute(typ_attributes);
                 addStamp(stamp, name, loc, Type(typ_type), docs);
               })
               /* addLocation(loc, typ_type, None); */
        | Tstr_module({
            mb_id: {stamp, name},
            mb_name: {loc},
            mb_expr: {
              mod_type,
              mod_desc:
                Tmod_structure(structure) |
                Tmod_constraint({mod_desc: Tmod_structure(structure)}, _, _, _)
            },
            mb_attributes
          }) =>
          let docs = PrepareUtils.findDocAttribute(mb_attributes);
          addStamp(
            stamp,
            name,
            loc,
            Module(stampNames(structure.str_items)),
            docs
          );
        | Tstr_module({mb_attributes, mb_id: {stamp, name}, mb_name: {loc}}) =>
          let docs = PrepareUtils.findDocAttribute(mb_attributes);
          addStamp(stamp, name, loc, Module([]), docs);
        /* | Tstr_modtype */
        | _ => ()
        }
      );
    let enter_core_type = typ =>
      /* open Typedtree; */
      /* Collector.add(~depth=depth^, typ.ctyp_type, typ.ctyp_loc); */
      switch typ.ctyp_desc {
      | Ttyp_constr(path, {txt, loc}, args) =>
        addLocation(loc, typ.ctyp_type, Path(path))
      /* if (usesOpen(txt, path)) {
           add_use((path, Type), txt, loc);
         };
         Collector.ident((path, Type), loc) */
      | _ => ()
      };
    let rec dig = typ =>
      switch typ.Types.desc {
      | Types.Tlink(inner) => dig(inner)
      | Types.Tsubst(inner) => dig(inner)
      | _ => typ
      };
    let enter_pattern = pat =>
      switch pat.pat_desc {
      | Tpat_alias(_, {stamp, name}, {txt, loc})
      | Tpat_var({stamp, name}, {txt, loc}) =>
        addStamp(stamp, name, loc, Value(pat.pat_type), None);
        addLocation(loc, pat.pat_type, IsDefinition);
      | Tpat_construct({txt, loc}, {cstr_name, cstr_loc, cstr_res}, args) =>
        switch (dig(cstr_res).Types.desc) {
        | Tconstr(path, args, _) =>
          addLocation(
            loc,
            pat.pat_type,
            Constructor(path, cstr_name, cstr_loc)
          )
        | _ => ()
        }
      | Tpat_record(items, isClosed) =>
        items
        |> List.iter(
             (({Asttypes.txt, loc}, {Types.lbl_res, lbl_name, lbl_loc}, value)) =>
             switch (dig(lbl_res).Types.desc) {
             | Tconstr(path, args, _) =>
               addLocation(loc, lbl_res, Attribute(path, lbl_name, lbl_loc))
             | _ => ()
             }
           )
      | _ => ()
      };
    let enter_expression = expr =>
      switch expr.exp_desc {
      | Texp_for({stamp, name}, {ppat_loc}, {exp_type}, _, _, contents) =>
        addLocation(ppat_loc, exp_type, IsDefinition);
        addScope(rangeOfLoc(contents.exp_loc));
        addStamp(stamp, name, ppat_loc, Value(exp_type), None);
        popScope();
      | Texp_ident(path, {txt, loc}, _) =>
        addLocation(loc, expr.exp_type, Path(path))
      | Texp_field(inner, {txt, loc}, {lbl_name, lbl_res, lbl_loc}) =>
        switch (dig(lbl_res).Types.desc) {
        | Tconstr(path, args, _) =>
          addLocation(loc, expr.exp_type, Attribute(path, lbl_name, lbl_loc))
        | _ => ()
        }
      | Texp_constant(_) =>
        addLocation(expr.exp_loc, expr.exp_type, IsDefinition)
      | Texp_record(items, ext) =>
        items
        |> List.iter(
             (({Asttypes.txt, loc}, {Types.lbl_loc, lbl_name, lbl_res}, ex))
             /* addLocation(loc, ex.exp_type, Location(lbl_loc)) */
             =>
               switch (dig(lbl_res).Types.desc) {
               | Tconstr(path, args, _) =>
                 addLocation(
                   loc,
                   ex.exp_type,
                   Attribute(path, lbl_name, lbl_loc)
                 )
               | _ => ()
               }
             )
      | Texp_construct({txt, loc}, {cstr_name, cstr_loc, cstr_res}, args) =>
        /* Huh, we can jump right to cstr_loc!! Wow */
        switch (dig(cstr_res).Types.desc) {
        | Tconstr(path, args, _) =>
          addLocation(
            loc,
            expr.exp_type,
            Constructor(path, cstr_name, cstr_loc)
          )
        | _ => ()
        }
      | Texp_let(recFlag, bindings, expr) => {
        let start = Asttypes.Recursive == recFlag ? (List.hd(bindings).vb_loc.loc_start) : expr.exp_loc.loc_start;
        addScope((posOfLexing(start), posOfLexing(expr.exp_loc.loc_end)))
      }
      | Texp_function(label, cases, _) => {
        addScope(rangeOfLoc(expr.exp_loc))
      }
      | _ => ()
      };

    let leave_expression = expr => switch expr.exp_desc {
      | Texp_let(recFlag, bindings, expr) => {
        popScope()
      }
      | Texp_function(_) => popScope()
      | _ => ()
    }
  };
  let process = cmt => {
    let data = {
      stamps: Hashtbl.create(100),
      /* references: Hashtbl.create(100), */
      exported: Hashtbl.create(10),
      topLevel: [],
      locations: []
    };
    module IterIter =
      TypedtreeIter.MakeIterator(
        (
          F(
            {
              let data = data;
            }
          )
        )
      );
    let structure = items => {
      let names = stampNames(items);
      names
      |> List.iter(((name, stamp)) =>
           Hashtbl.replace(data.exported, name, stamp)
         );
      data.topLevel = names;
      List.iter(IterIter.iter_structure_item, items);
    };
    let iter_part = part =>
      switch part {
      | Cmt_format.Partial_structure(str) => {IterIter.iter_structure(str); stampNames(str.str_items)}
      | Partial_structure_item(str) => {IterIter.iter_structure_item(str); stampNames([str])}
      | Partial_signature(str) => {IterIter.iter_signature(str); []}
      | Partial_signature_item(str) => {IterIter.iter_signature_item(str); []}
      | Partial_expression(expression) => {IterIter.iter_expression(expression); []}
      | Partial_pattern(pattern) => {IterIter.iter_pattern(pattern); []}
      | Partial_class_expr(class_expr) => {IterIter.iter_class_expr(class_expr); []}
      | Partial_module_type(module_type) =>
        {IterIter.iter_module_type(module_type); []}
      };
    switch cmt {
    | Cmt_format.Implementation(str) => structure(str.str_items)
    | Cmt_format.Interface(sign) => IterIter.iter_signature(sign)
    | Cmt_format.Partial_implementation(parts)
    | Cmt_format.Partial_interface(parts) => {
      let names = Array.map(iter_part, parts) |> Array.to_list |> List.concat;
      names
      |> List.iter(((name, stamp)) =>
           Hashtbl.replace(data.exported, name, stamp)
         );
      data.topLevel = names;
    }
    | _ => failwith("Not a valid cmt file")
    };
    data.locations = List.rev(data.locations);
    data;
  };
};

let process = Get.process;

open Infix;

let checkPos =
    (
      (line, char),
      {Location.loc_start: {pos_lnum, pos_bol, pos_cnum}, loc_end}
    ) =>
  Lexing.(
    if (line < pos_lnum || line == pos_lnum && char < pos_cnum - pos_bol) {
      false;
    } else if (line > loc_end.pos_lnum
               || line == loc_end.pos_lnum
               && char > loc_end.pos_cnum
               - loc_end.pos_bol) {
      false;
    } else {
      true;
    }
  );

let locationAtPos = ((line, char), data) => {
  let pos = (line + 1, char);
  let rec loop = locations =>
    switch locations {
    | [] => None
    | [(loc, expr, defn), ..._] when checkPos(pos, loc) =>
      Some((loc, expr, defn))
    | [_, ...rest] => loop(rest)
    };
  loop(data.locations);
};

let maybeFound = (fn, a) =>
  switch (fn(a)) {
  | exception Not_found => None
  | x => Some(x)
  };

let findDefinition = (defn, data) =>
  /* Log.log("😍 resolving a definition"); */
  switch defn {
  | IsDefinition =>
    Log.log("Is a definition");
    None;
  | Constructor(path, _, _)
  | Attribute(path, _, _)
  | Path(path) =>
    switch path {
    | Path.Pident({stamp: 0, name}) =>
      Some(`Global(name, []))
    | Path.Pident({stamp, name}) =>
      maybeFound(Hashtbl.find(data.stamps), stamp) |?>> x => `Local(x)
    | Path.Pdot(inner, name, _) =>
      let rec loop = p =>
        switch p {
        | Path.Pident({stamp: 0, name}) => {
          `Global(name, [])
        }
        | Path.Pident({stamp, name}) =>
          `Local(maybeFound(Hashtbl.find(data.stamps), stamp))
        | Path.Pdot(inner, name, _) =>
          switch (loop(inner)) {
          | `Global(top, subs) => `Global(top, subs @ [name])
          | `Local(Some((_, _, Module(contents), _, _))) =>
            `Local(maybeFound(List.assoc(name), contents)
            |?> maybeFound(Hashtbl.find(data.stamps)))
          | _ => `Local(None)
          }
        | _ => `Local(None)
        };
      switch (loop(inner)) {
      | `Global(top, children) => Some(`Global(top, children @ [name]))
      | `Local(Some((_, _, Module(contents), _, _))) =>
        maybeFound(List.assoc(name), contents)
        |?> maybeFound(Hashtbl.find(data.stamps))
        |?>> x => `Local(x)
      | _ =>  None
      };
    | _ =>
      None
    }
  };

/* let resolveDefinition = (defn, data) => switch (findDefinition(defn, data)) {
| None => None
| Some(`Global(top, children)) => {
}
| Some(`Local(defn)) => Some(defn)
}; */
