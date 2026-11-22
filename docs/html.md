# Pages: HTML as Forth, or as templates

A page, or a part of one, is a word that adds its HTML to the string being built (`build`, docs/forth.md: sequences)
- no tree of nodes, no virtual DOM. A part is rendered again by running its word again. Two ways to write one, which
make the same text and call each other: Forth words (`html`), or HTML files with holes compiled into Forth words
(`html.templates`). Everything a user wrote is escaped unless it is put in with `raw`.

## In Forth (`html`)

```forth
USING: html json ;
: task-row ( task -- )
    {: t :}
    <tr  s" row" class=
        <td  s" title" t json-string@ drop text  td>
        <td  <input  s" checkbox" type=  s" done" t json-bool@ drop checked=  td>
    tr> ;
: task-table ( tasks -- )  {: ts :}  <table  ts [: task-row ;] each  table> ;
[: tasks task-table ;] html   ( -- s )
```

- `<tag` (every HTML element: `<div <table <tr <td <form <button <a <ul <li <h1 ...`) begins an element; its
  attributes come next, before its content; `tag>` ends it. A void element (`<input <br <img <meta <link <hr ...`)
  has no ending word. (Not `<u`: its `u>` would hide the core's unsigned comparison - `s" u" open-element ... s" u"
  close-element`, as for any other.)
- Attributes: `id= class= href= src= type= name= value= placeholder= for= action= method= title= alt= rel= style=
  target= rows= cols= min= max= step= pattern= ...` ( value n -- ), and `attr= ( value n name n -- )` for any other.
  Flag attributes take a flag - `checked= selected= disabled= required= readonly= autofocus= multiple= hidden= open=`
  ( flag -- ): there if it is true, so a form is drawn from its data. An attribute after the element's content has
  begun is an error.
- Content: `text ( a n -- )` escaped (`& < > " '`), `text-of ( s -- )` a string object's, `number ( n -- )`,
  `raw ( a n -- )` as it is - only markup you made, never what a user wrote. `doctype`. Attribute values are escaped.
- `html ( xt -- s )`: `build` for HTML - the code's elements, as a string (its last start tag closed).
- `%escaped ( a n -- )` adds escaped text to any build; `html-escape ( a n -- s len )` makes an escaped copy.
- A component is a word; a layout is a word given the content as a quotation (`[: task-table ;] s" Tasks" layout`).

## As templates (`html.templates`)

```html
<!-- views/task-row.html: the word task-row ( v -- ) -->
<tr id="task-{id}" class="{#if done}done{/if}">
  <td>{title}</td>
  <td>{owner.name}</td>
  <td>{#if done}done{#else}open{/if}</td>
</tr>
```

```html
<!-- views/task-table.html: the word task-table ( v -- ) -->
<table>{#each}<task-row/>{/each}</table>
```

```forth
USING: html html.templates ;
templates views                  \ every .html in views/ (beside this file): a word each
[: tasks task-table ;] html
```

- `{name}`: a field of the value (a JSON object, as `json>` gives), escaped - `{a.b}` a field of a field, `{.}` the
  value itself. A string as it is, a whole number in decimal, `true` as `true`; false, null and a field not there as
  nothing; a list or an object is an error (give a field of it). In attribute values too.
- `{#each name}...{/each}`: each item of the list (the item the value inside); `{#each}` the value's own items.
- `{#if name}...{#else}...{/if}`: the field there and not false, null, 0, "" or an empty list.
- `<task-row/>`: another template's word, or any word `( v -- )` that writes HTML, given the value - a name with a
  hyphen (a custom element's form) and nothing else in the tag. (The rest of a template's code names its words
  whole - `html:raw`, `forth:each` - so the file loading it needs only `html.templates`, and its own words change
  nothing.)
- `{{` and `}}`: braces. **Nothing else** - no Forth in a template: logic lives in words, and a template is HTML with
  holes, read at a glance.
- `templates dir` (or `templates-in ( dir n -- )`): every `.html` in the folder, a word each, named after its file
  without `.html` - all declared first, so one may use another whatever their order. `template path` (or
  `template-file ( path n -- )`) one file; `template-text ( text n name n -- )` from text. Paths are from the folder
  of the file being read. A template said wrong is an error naming its file and line (`template views/row.html:3: a
  block not closed`). `reload` compiles them again.
