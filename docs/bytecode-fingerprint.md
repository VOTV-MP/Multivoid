# Bytecode fingerprints

## Purpose

A game update can keep every class and function name the mod uses and still change what a function
does. Resolving names by string cannot see that. A fingerprint can: the mod renders a game function's
compiled Blueprint bytecode as canonical text and hashes it, so the same function on two builds of the
game compares equal when its logic is the same and differs when it is not. This page is the
specification of that text. The mod and the port tooling each render it, one from the running game and
one from the cooked assets, and the two renderings are compared function by function.

## How it works

### Reading the bytecode

A Blueprint function's script is the byte array `UStruct::Script`. The mod walks it expression by
expression with a decoder of every UE4.27 Kismet opcode, laid out as the engine keeps it in memory: an
object or property pointer takes 8 bytes and a name (`FScriptName`) 12. The operand order of each
opcode, and its name, follow UAssetAPI's expression readers (MIT): an expression is written with the
name of its `EExprToken` less `EX_`. A decoder that mis-sized one operand would not land on the script's
end; on both game builds it has read every function of every loaded Blueprint class to its end with no
problem `[V]` (every function of a capture: 21,087 on 0.9.0n, 22,762 on 0.10.0, in a world). Expressions nested more than
256 deep are refused as a corrupt script; a compiled Blueprint nests a few dozen deep.

### The canonical text

1. **Line 1 is the signature**: `sig(` then the function's parameters (the properties flagged
   `CPF_Parm`) in declaration order, comma-separated, each `name:flags:type`, then `)` and a newline.
   The flags are letters in the order C (const), P (parameter), O (out), R (return), F (reference). The
   type is the name of the property's class (`objectproperty`, `intproperty`, ...), followed by:
   - `:<object path>` of the property's type object, when it has one, for an object, soft object, weak
     object, lazy object, class or soft class property (the class it holds: `class` for a class
     property), an interface property (its interface), a struct property (its struct), a byte property
     (its enum, if it has one), and a delegate or any multicast delegate property (its signature
     function);
   - for an enum property, `:<enum path>`;
   - for an array or set property `<inner type>`, for a map property `<key type,value type>`, each
     written by the same rule, to a depth of four (deeper, the class name alone).
   There is no size: a native struct's cooked size is not its run-time one (`FHitResult` reads 144 bytes
   in the asset and 136 in the game `[V]`), and the type already names the struct. A native function has
   no script, so its text is this line alone.
2. **Statements in flow order**, one per line as `<label> <expression>`. Every statement of the script is
   read first, from offset 0 to its end. From the entry a straight run is followed: an unconditional
   `EX_Jump` is layout glue, followed and never written, and `Jump`, `Return`, `EndOfScript`,
   `PopExecutionFlow` and `ComputedJump` end a run. Every other code offset an expression holds -- a
   conditional branch, a pushed execution flow, a latent action's resume point -- is deferred, last in
   first out, in operand order. A run that reaches a statement already written ends with
   ` >@<label>`. Labels are indices in this order, and every code offset is written `@<label>` (`@?` if
   it is not a statement). A recook that re-lays a function out without changing it therefore writes the
   same text.
3. **An expression** is `Name(operand,...)`, or `Name` alone when it has no operand. Operands, in the
   reader's order:
   - `o:<object path>`: the object's outer chain, without its package, joined by `.` (`o:-` for null);
   - `p:<owner path>:<field chain>` for a property: the path of the first object that owns it, then the
     property names from the outermost field in, joined by `.` (`p:-` for null); `l<k>:<base>` for a
     frame's own property (rule 4);
   - `n:<name>`; the integers in decimal, `i:` (int32), `b:` (byte), `h:` (uint16), `l:` (int64), `u:`
     (uint64), and `B:0`/`B:1`; `f:` a float's 32 bits as 8 lowercase hex digits, `d:` a double's as
     16; `s:"..."` a string; `[...]` a list, comma-separated.
   Dropped, as derived from the layout: `EX_Context`'s skip offset, `EX_Skip`'s offset,
   `EX_SwitchValue`'s goto offsets and the element counts of container constants. `StructConst` keeps
   its struct's serialized size as the script writes it (`i:`). The expressions with a form of their own:
   - `TextConst(t:<kind>,...)`: the kind is `Empty`, `LocalizedText` (then its source, key and namespace
     expressions), `InvariantText` or `LiteralString` (one expression), or `StringTableEntry` (then
     `o:<table>`, its id and its key);
   - `SwitchValue(<index>,[(<value>,<result>),...],<default>)`;
   - `PrimitiveCast(k:<conversion>,...)`: `ObjectToInterface` (then `o:<interface>`), `ObjectToBool`,
     `InterfaceToBool`, or `?<number>` for another, then the cast expression;
   - `VectorConst` and `RotationConst` (pitch, yaw, roll) write their three floats, `TransformConst` its
     ten (rotation x, y, z, w, translation x, y, z, scale x, y, z), each `f:`;
   - `InstrumentationEvent(b:<type>)`, and for an inline event (type 4) `,n:<name>` (only a build
     compiled with instrumentation carries one).
4. **A frame's own property** -- one whose owner is a function: a parameter, or a slot the compiler made
   (`CallFunc_*`, `K2Node_*`, `Temp_*`) -- is written `l<k>:<base>`: `k` by first appearance in the text
   (restarting where rule 6 says), `base` its own name with every `_<digits>` part after the first taken
   out. The compiler numbers its slots across a whole event graph and a plugin node numbers inside the
   name (`K2Node_LowEntry_LocalVariable_Value__12_Object`), so an edit elsewhere renumbers both.
5. **Text.** Names, paths and types are folded to printable ASCII and lowercased: a name renders in the
   casing first registered in the process, which the game cannot know from the asset. A name with a
   number writes it as `_<number - 1>`, as the engine does. A string keeps its case; any character
   outside printable ASCII becomes `?`, one per UTF-16 unit. In a name, a path and a string alike, a `"`
   or a `\` is written with a `\` before it.
6. **Events.** An event function is a stub that calls its class's event graph
   (`ExecuteUbergraph_<class>`) at an entry offset. The call is written
   `<call name>(o:<event graph path>,[IntConst(@U),<its other arguments>])`, and the text continues with
   `== <event graph path> ==` and the event graph's statements in flow order from that entry, labels
   restarting, frame numbering shared with the stub. An `ExecuteUbergraph_*` function itself is written as
   `== entry <stub> ==` and its flow from each stub's entry, the stubs of its class sorted by their
   rendered name (then entry offset), frame numbering restarting for each. If any function of the class
   does not decode, the event graph is undecodable: a stub left out would be a region left out.
7. **Fingerprints** are FNV-1a 64 over the UTF-8 text: one over the signature line, its newline
   included (a change means a hook reads wrong parameters), and one over the rest (a change means the
   logic drifted).

### Agreement

The text is rendered twice: by the mod from the running game, and by the port tooling from the cooked
assets (UAssetAPI's expression tree). A difference between the two is a defect in one of the readers.
Over every loaded Blueprint function the cooked assets also hold, the two agree `[V]`: on 0.9.0n 21,008
of 21,068 captured are identical and none differs; the other 60 have no counterpart among the pak's
Blueprint classes (48 natives, five native event declarations such as `actor.receivetick`, and the
seven functions of the level's own script class, which lives in the map). On 0.10.0, 22,744 of 22,756
are identical and none differs; the other 12 are the level's script class and a function library outside
the pak's Blueprints.

### The registry

Every game function the mod watches or hooks is a **row**, declared once in the file that registers it:

    UE_FACT_FN(kRowFix, "serverbox_sync", L"serverBox_C", L"fix", Declared);

with the lane that relies on it (a lowercase id), the class (`nullptr` for every class's function of that
name), the function and how it resolves: `Declared` (the class declares it), `Dispatch` (the class or its
nearest ancestor), `AnyClass` (a name watch on every class), `RuntimeClass` (the class is known only at
run time; the site resolves it with `facts::ResolveOn`, which records the class's name for the census).
The registration calls of `script_gate`, `ufunction_hook` and `game_thread` accept only a row or a
`facts::Resolved` made from one, so a dependency cannot be added without its row, and each row records
whether its registration was taken or refused. A function a constant the mod writes depends on (the string a game spawner
stamps into an actor before its BeginPlay reads it) is a row too, resolved by the site that writes the constant,
though nothing hooks it, so a build that moved the function is named by the census.

### The census

The census starts once a process, in its first gameplay world, however the player got there -- a load or
a join from the menu, or a test launcher's scenario -- and checks every row against the **reference
table** of the build the mod was made for (`include/ue_wrap/facts/fingerprints_<target>.inc`, generated
by the port tooling from that build's pak and the mod's capture on it). A new row needs its entry there:
the maintainer regenerates the table, and the source gate refuses a push whose rows it does not key.

A class row is resolved as its registration resolves it and fingerprinted. A `RuntimeClass` row with no
class is keyed on the class its site resolved; the table carries an entry for every class that declares
its function, and the row's key in the report is `*.<function>`. An any-class row checks the signature of
every function of that name against the reference's set as its class loads: the object index logs every
class it lists, the census reads each class's functions once from where it stopped, and the row stays
open for the classes a session loads later, so no walk of the object array is ever repeated.

Each row ends as `match` (an any-class row: `holds`), `sig-changed` (a hook reads wrong parameters),
`body-changed` (the logic drifted), `appeared` (present, though the reference build lacks it), `gone`
(absent though the reference has it), `known-gone` (absent on the reference build too), `unchecked` (its
class is not loaded, or no function of an any-class row's name is; off the reference build a class not
loaded and a class gone cannot be told apart, and the row says so; a class row is tried again when the
class set moves and every 5 s, an any-class row when a class loads), `undecodable`, or `no-reference`
(the table cannot judge it: a run-time class it has no entry for). Every outcome but `match`, `holds`,
`known-gone` and `unchecked` is a problem.

The work is sliced at 2 ms, one slice a runtime loop, in a game-thread task of its own so that another
lane's fault in the per-frame work cannot keep it from finishing, with each report in a task of its own.
A slice reads nothing while the object index has the engine's events still to apply, and a class
unloading during a pass drops the pass's caches, so no function is read through a pointer its class took
with it. A pass reads the 423 rows in about 45 ms of work, its worst slice one whole-event-graph row at 6
to 8.5 ms `[V]`.

`multivoid-compat-report.txt` carries one line per row --
`<outcome>  <lane>  <mode>  <key> -> <declaring function>  live <sig>/<body>  ref <sig>/<body>`, then
`(not registered this session)` or `(registration refused)` where so, and `-- <detail>` -- with a line
saying how many listed classes the any-class rows have met, how many were gone before they were met (a
class can load with the world and be dropped first, so an any-class row's answer can differ from run to
run) and which are still loading, and the summary. The file opens with the game build that runs and the
boot's engine checks (written at start, so a session that never reaches a world has a report too) and
ends in one verdict over every section: every census problem row counts as an important failure, and
until the census has reported the verdict reads PENDING, never HEALTHY. The health log gets the summary,
`fingerprint-census: N of M class rows match, ...`, with the costliest rows and the met line, when the
first pass ends, when a product row's outcome moves, and when a registration is refused; a count that
moves, a dev row, or a registration accepted (at most once a second) rewrites the file alone. Each
problem row and each refused registration warns once, naming its lane; rows of dev instruments are
counted apart, out of the verdict, and warn only while their instrument has registered them. Measured on
0.10.0 against the 0.9.0n reference, the census named 42 problem rows by lane -- three of them functions
whose only change is statements trading places (the known limit below) -- and agreed with the port
tooling's diff of the two builds' paks on all 266 rows both could judge `[V]`.

`[dev] fingerprint_census_red=<class>.<function>` falsifies one row's reference: the census must report
it `body changed` and print `fingerprint-census: RED ARM <row> reported body changed` once the row is
settled, or `RED ARM FAILED <row>` -- also at once when no row has that key or the key names an any-class
row. While the row is not settled it says once `fingerprint-census: RED ARM <row> waits for its row`.

### The capture

`[dev] fingerprint_capture` writes, once the census's first pass has met the loaded classes, every
function of every loaded Blueprint class, every row's resolved function and every function an any-class
row names, natives included, with its fingerprint and text, sorted by path, to
`multivoid-fingerprints-capture.txt` beside the game's exe: `#row <lane> <mode> <key> -> <resolved
function>` lines first, then for each function `### <path> <fingerprint> <ok|native|problem>` and its
text, where the fingerprint is FNV-1a 64 over the whole text in 16 hex digits and a path may hold a
space. It is about a second of game-thread work.

## Who owns what

`ue_wrap/facts` holds the rows and resolves them; `ue_wrap/core/script_bytecode` decodes;
`ue_wrap/core/script_fingerprint` orders, renders the signature and hashes; `harness/fingerprint_census`
runs the census, `harness/fingerprint_capture` the capture; `harness/compat_report` writes the report
file and its verdict, which `sdk_check` and the census both feed. A source gate
(`.github/ci/fact_rows_gate.py`, on every push) holds every row well formed and present in the target's
reference table (a run-time class row with no class: on at least one class). All of it reads; none of it
changes the game.

## Wire messages

None: a fingerprint is a local check of the game the mod runs on.

## Late join

Not applicable: every peer checks its own game, once a process, in its first gameplay world.

## Known limits

- Two statements that swap places inside one straight run read as a change although nothing depends on
  their order: 19 of 362 functions that differ between 0.9.0n and 0.10.0 in the hooked Blueprints `[V]`.
  It errs toward reporting.
- A native function is compared by its signature; its body is not readable.
- Only loaded classes are read in the game; the port tooling reads every class of the pak.
- An any-class row is checked by signature only: its overrides' bodies are many, and a name watch depends
  on the parameters it reads and on who calls it.

## Code map

| File | What |
|---|---|
| `src/votv-coop/src/ue_wrap/core/script_bytecode.cpp` | the decoder: the opcode table, one expression scanned or rendered, a script's statements |
| `src/votv-coop/src/ue_wrap/core/script_fingerprint.cpp` | flow order, the signature line, the fingerprints, and `script_calls::EnumerateCalls`, the call expressions in the same order |
| `src/votv-coop/include/ue_wrap/facts/function_row.h` | the row macro, the modes, `Resolve` and `ResolveOn` |
| `src/votv-coop/include/ue_wrap/facts/fingerprints_<target>.inc` | the generated reference table |
| `src/votv-coop/src/harness/fingerprint_census.cpp` | the census |
| `src/votv-coop/src/harness/fingerprint_capture.cpp` | the capture |
| `src/votv-coop/src/harness/compat_report.cpp` | the report file's one writer |
| `.github/ci/fact_rows_gate.py` | the rows' source gate |
