# mini-sql

A SQLite-style database engine, built from scratch in C — one layer at a time.

> Small. Schema-driven. Honest about what it isn't (yet).

`mini-sql` is a teaching/learning implementation that follows the spirit of
[cstack's *Let's Build a Simple Database*](https://cstack.github.io/db_tutorial/),
but **deliberately diverges in one important way**: where the tutorial hardcodes
a single `Row` struct and compile-time byte offsets, `mini-sql` is **schema-driven
from the first commit**. The on-disk/in-memory layout of a row is *computed at
runtime* from a `Schema`, not frozen by the C compiler. That single decision is
what keeps the door open to `CREATE TABLE`, `ALTER TABLE`, and multiple tables
without a rewrite.

Today it is an in-memory, single-table store with a validating insert parser and
a black-box test suite. The storage layer underneath is the shape the future
pager and B-tree will plug into.

---

## Table of contents

- [Quick start](#quick-start)
- [What works today](#what-works-today)
- [Architecture at a glance](#architecture-at-a-glance)
- [How it maps to SQLite](#how-it-maps-to-sqlite)
- [Module dependency graph](#module-dependency-graph)
- [The data model](#the-data-model)
- [Row layout & storage](#row-layout--storage)
- [Command lifecycle](#command-lifecycle)
- [The REPL state machine](#the-repl-state-machine)
- [Insert validation](#insert-validation)
- [Memory ownership](#memory-ownership)
- [Build system](#build-system)
- [Testing](#testing)
- [Design decisions](#design-decisions)
- [Roadmap](#roadmap)
- [Project layout](#project-layout)
- [Limitations & non-goals](#limitations--non-goals)

---

## Quick start

```sh
cmake -S . -B build          # configure
cmake --build build          # compile
./build/mini_sql             # run the REPL
ctest --test-dir build       # run the test suite
```

A session:

```text
db > insert 1 alice alice@example.com
Executed. (0.004 ms)
db > insert 2 bob bob@example.com
Executed. (0.003 ms)
db > select
(1, alice, alice@example.com)
(2, bob, bob@example.com)
Executed. (0.002 ms)
db > insert -1 carol carol@example.com
ID must be positive.
db > .exit
```

Requirements: a C11 compiler (Apple Clang / GCC), CMake ≥ 3.20, and `bash` for the
tests. No third-party libraries.

---

## What works today

| Capability | Status |
| --- | --- |
| Interactive REPL with `db >` prompt | ✅ |
| Meta-commands (`.exit`) | ✅ |
| `insert <id> <username> <email>` into a fixed `users` schema | ✅ |
| `select` (full scan, prints all rows) | ✅ |
| Input validation (syntax, negative id, over-length text, table full) | ✅ |
| Schema-driven row (de)serialization | ✅ |
| Per-statement execution timing | ✅ |
| Black-box test suite via CTest | ✅ |
| Persistence to disk | ⛔ in-memory only |
| B-tree / ordered access | ⛔ append-only array |
| `CREATE TABLE` / multiple tables | ⛔ single hardcoded schema |

---

## Architecture at a glance

The engine is a classic **front-end / back-end** split. The front end turns text
into a validated `Statement`; the back end executes it against storage. Everything
above the storage layer is stateless; all mutable state lives in a `Table`.

```mermaid
flowchart TD
    User([User]) -->|SQL & meta-commands| REPL

    subgraph Interface["Interface layer"]
        REPL["main.c — REPL loop"]
        IB["input_buffer — line reader"]
    end

    subgraph Frontend["Front end — compile & validate"]
        META["meta_command — dot-commands"]
        STMT["statement — tokenize + validate"]
    end

    subgraph Backend["Back end — execute & store"]
        EXEC["executor — insert / select"]
        TABLE["table — pages of rows"]
        REC["record — row (de)serialization"]
        SCHEMA["schema — column layout"]
    end

    REPL --> IB
    REPL --> META
    REPL --> STMT
    REPL --> EXEC
    STMT --> REC
    STMT --> SCHEMA
    EXEC --> TABLE
    EXEC --> REC
    TABLE --> SCHEMA
    REC --> SCHEMA
    META --> TABLE
```

The guiding principle: **`schema` sits at the bottom of the dependency graph and
everything reads its layout decisions.** `record` knows *how to move bytes*;
`schema` knows *where the bytes go*. Keeping those two concerns apart is what makes
the row format mutable later.

---

## How it maps to SQLite

SQLite compiles SQL to bytecode and runs it on a virtual machine over a B-tree/pager
stack. `mini-sql` builds the **bottom-right of that diagram** first (storage), and
keeps the "compiler" intentionally trivial (no bytecode VM — the executor runs the
statement directly).

```mermaid
flowchart LR
    subgraph SQLite["SQLite"]
        direction TB
        s1[Tokenizer]
        s2[Parser]
        s3[Code Generator]
        s4[Virtual Machine / VDBE]
        s5[B-Tree]
        s6[Pager]
        s7[OS Interface / VFS]
    end
    subgraph Mini["mini-sql today"]
        direction TB
        m1["statement.c — strtok tokenizer"]
        m2["statement.c — keyword dispatch"]
        m3["(none — no parse tree)"]
        m4["executor.c — direct execution"]
        m5["table.c — array of pages"]
        m6["(none yet — in-memory)"]
        m7["stdio + malloc"]
    end
    s1 -.-> m1
    s2 -.-> m2
    s3 -.-> m3
    s4 -.-> m4
    s5 -.-> m5
    s6 -.-> m6
    s7 -.-> m7
```

The dotted lines that land on `(none)` are the honest gaps — they are also the
[roadmap](#roadmap).

---

## Module dependency graph

Each `.c` includes only the headers it truly needs; `schema` and `input_buffer` are
the leaves. The static library `mini_sql_lib` contains every module except `main`,
so tests and future benchmarks link against it without recompiling sources.

```mermaid
flowchart BT
    main --> input_buffer
    main --> meta_command
    main --> statement
    main --> executor
    main --> schema
    main --> table

    meta_command --> input_buffer
    meta_command --> table
    statement --> input_buffer
    statement --> record
    executor --> statement
    executor --> table
    record --> schema
    table --> schema

    classDef leaf fill:#e8eefc,stroke:#5577cc;
    class schema,input_buffer leaf;
```

| Module | Responsibility | Key entry points |
| --- | --- | --- |
| `input_buffer` | Read a line of stdin into a growable buffer | `new_input_buffer`, `read_input`, `close_input_buffer` |
| `meta_command` | Handle `.`-prefixed commands; teardown on `.exit` | `do_meta_command` |
| `schema` | Runtime column layout: types, sizes, **computed offsets** | `schema_create`, `schema_find_column_by_id/name`, `schema_free` |
| `record` | An opaque row payload + schema-keyed get/set + (de)serialize | `record_init`, `record_set_int/text`, `serialize_record`, `print_record` |
| `table` | Page-backed row storage; maps row number → byte slot | `table_new`, `table_row_slot`, `table_free` |
| `statement` | Tokenize + validate input into a `Statement` | `prepare_statement`, `statement_set_default_schema` |
| `executor` | Run a prepared `Statement` against a `Table` | `execute_statement` |
| `main` | REPL loop + wiring + lifetime management | — |

---

## The data model

Five structs carry all state. Note the relationship semantics — they encode the
ownership rules the code actually follows:

- `Schema` **owns** its `ColumnDefinition` array (deep copy, including column names).
- `Table` **borrows** a `Schema` (a non-owning pointer; `main` controls its lifetime).
- `Statement` **holds** a `Record` inline.
- `Record` is just bytes; it is **interpreted by** a `Schema` but does not own one.

```mermaid
classDiagram
    class Schema {
        +uint32 version
        +uint32 next_column_id
        +uint32 num_columns
        +ColumnDefinition columns
        +uint32 row_size
    }
    class ColumnDefinition {
        +uint32 column_id
        +string name
        +ColumnType type
        +uint32 size
        +uint32 offset
    }
    class Record {
        +pointer payload
        +uint32 payload_size
    }
    class Table {
        +Schema schema
        +uint32 num_rows
        +pages100 pages
    }
    class Statement {
        +StatementType type
        +Record record_to_insert
    }

    Schema "1" *-- "many" ColumnDefinition : owns
    Table "1" o-- "1" Schema : borrows
    Statement "1" *-- "1" Record : holds
    Record ..> Schema : interpreted by
```

The pivotal field is `ColumnDefinition.column_id` — a **stable identity** assigned
once and never reused. Every read/write addresses a column by `column_id`, never by
its position or its name. That indirection is what will make `ALTER TABLE RENAME
COLUMN` a one-line metadata change instead of a code-wide find-and-replace.

---

## Row layout & storage

`schema_create` walks the column list once, assigning each column a byte `offset`
and accumulating `row_size`. For the built-in `users` schema:

| Column | Type | Size (bytes) | Offset |
| --- | --- | ---: | ---: |
| `id` | `INT` | 4 | 0 |
| `username` | `TEXT` | 32 | 4 |
| `email` | `TEXT` | 255 | 36 |
| **`row_size`** | | **291** | |

A `Record`'s `payload` is exactly that 291-byte block:

```mermaid
flowchart LR
    subgraph payload["Record.payload — 291 bytes"]
        direction LR
        idf["id<br/>int32<br/>bytes 0-3"]
        unf["username<br/>text<br/>bytes 4-35"]
        emf["email<br/>text<br/>bytes 36-290"]
    end
    idf --- unf --- emf
```

`table_row_slot` packs rows into 4 KiB pages, allocated lazily on first touch.
**Rows never straddle a page boundary** — a simplifying invariant that makes a row's
address a pure function of its row number.

```mermaid
flowchart TD
    T["Table (num_rows, schema*)"] --> PTR["pages[100] — pointers, NULL until used"]
    PTR --> PG0
    PTR --> PG1
    PTR --> PGN["page 99"]

    subgraph PG0["page 0 — 4096 bytes"]
        r0["row 0 (291 B)"]
        r1["row 1 (291 B)"]
        rd["… 14 rows total"]
        sl["22 B unused slack"]
    end
    subgraph PG1["page 1 — 4096 bytes"]
        rr["rows 14–27 …"]
    end
```

The paging math:

```text
rows_per_page = floor(PAGE_SIZE / row_size) = floor(4096 / 291) = 14
slack_per_page = 4096 - (14 × 291)          = 22 bytes
max_rows       = rows_per_page × MAX_PAGES  = 14 × 100 = 1400
```

Insert #1401 returns `EXECUTE_TABLE_FULL`. Because `rows_per_page` is derived from
`schema->row_size` at call time (not a compile-time constant), this all recomputes
automatically the day the schema changes.

---

## Command lifecycle

### INSERT

The parser validates **before** allocating, so a rejected insert never leaks the
record payload. Only after every token passes does it build the `Record`.

```mermaid
sequenceDiagram
    actor U as User
    participant M as main (REPL)
    participant S as statement
    participant Sc as schema
    participant R as record
    participant E as executor
    participant T as table

    U->>M: insert 1 alice a@x.com
    M->>S: prepare_statement(buf, &stmt)
    activate S
    S->>S: prepare_insert() — strtok per column
    loop each column (validate only)
        S->>Sc: column type + size
        Sc-->>S: ColumnDefinition*
    end
    S->>R: record_init + record_set_int/text
    S-->>M: PREPARE_SUCCESS
    deactivate S

    M->>E: execute_statement(&stmt, table)
    activate E
    E->>T: table_row_slot(table, num_rows)
    T-->>E: void* slot
    E->>R: serialize_record(rec, slot)
    E-->>M: EXECUTE_SUCCESS
    deactivate E

    M->>R: record_free(&stmt.record_to_insert)
    M-->>U: Executed. (0.003 ms)
```

### SELECT

A full scan: each row is deserialized into a temporary `Record`, printed, and freed.

```mermaid
sequenceDiagram
    actor U as User
    participant M as main
    participant E as executor
    participant T as table
    participant R as record

    U->>M: select
    M->>E: execute_statement(&stmt, table)
    activate E
    loop i in 0 .. num_rows-1
        E->>T: table_row_slot(table, i)
        T-->>E: void* slot
        E->>R: deserialize_record(slot, &rec, schema)
        E->>R: print_record(&rec, schema)
        E->>R: record_free(&rec)
    end
    E-->>M: EXECUTE_SUCCESS
    deactivate E
    M-->>U: (rows…) + Executed.
```

---

## The REPL state machine

```mermaid
stateDiagram-v2
    [*] --> Prompt
    Prompt --> Read : print "db > "
    Read --> Classify : getline

    Classify --> Meta : line starts with '.'
    Classify --> Prepare : otherwise

    Meta --> Prompt : success / unrecognized
    Meta --> [*] : .exit (frees table + schema)

    Prepare --> Execute : PREPARE_SUCCESS
    Prepare --> Prompt : syntax / negative-id / too-long / unrecognized

    Execute --> Prompt : EXECUTE_SUCCESS / EXECUTE_TABLE_FULL
```

The loop is infinite by construction; the only exit is `.exit`, which frees the
`Table` and `Schema` and calls `exit()` from inside `do_meta_command`. There is no
fall-through cleanup path because control never reaches the end of `main`.

---

## Insert validation

`prepare_insert` is **schema-generic** — it loops over `schema->columns` and
dispatches on each column's `type`. It is not hardcoded to three columns, so it will
parse inserts for any future runtime schema unchanged. The two-pass structure
(validate everything, *then* allocate) is what keeps the error paths leak-free.

```mermaid
flowchart TD
    A["insert line"] --> B["strtok: pull 1 token per column"]
    B --> C{token missing?}
    C -->|yes| E1["return PREPARE_SYNTAX_ERROR"]
    C -->|no| D{column type}
    D -->|INT| F{value negative?}
    F -->|yes| E2["return PREPARE_NEGATIVE_ID"]
    F -->|no| G["stash token"]
    D -->|TEXT| H{too long for column?}
    H -->|yes| E3["return PREPARE_STRING_TOO_LONG"]
    H -->|no| G
    G --> I{more columns?}
    I -->|yes| B
    I -->|no| J["record_init + set every column"]
    J --> K["return PREPARE_SUCCESS"]
```

| Result code | Message | Cause |
| --- | --- | --- |
| `PREPARE_SUCCESS` | — | well-formed insert |
| `PREPARE_SYNTAX_ERROR` | `Syntax error. Could not parse statement.` | too few tokens |
| `PREPARE_NEGATIVE_ID` | `ID must be positive.` | an `INT` column got a negative value |
| `PREPARE_STRING_TOO_LONG` | `String is too long.` | a `TEXT` value exceeds the column width |
| `PREPARE_UNRECOGNIZED_STATEMENT` | `Unrecognized keyword …` | not `insert`/`select` |

> Note: "negative id" is currently generalized to *any* `INT` column. That is a
> pragmatic stand-in until per-column constraints (PRIMARY KEY / UNSIGNED) exist —
> see [design decisions](#design-decisions).

---

## Memory ownership

Explicit ownership is the spine of a C codebase. The rules, drawn:

```mermaid
flowchart TD
    main ==>|owns| schema
    main ==>|owns| table
    main -.->|stack value| stmt["Statement"]

    schema ==>|owns: malloc + strdup| cols["columns[] + names"]
    table -.->|borrows| schema
    table ==>|owns: lazy malloc| pages["page buffers"]
    stmt ==>|owns: record_init| payload["Record.payload"]
```

Legend: **thick arrow = owns/frees**, **dotted arrow = borrows or stack**.

Lifecycle rules:

- `schema_create` deep-copies the caller's column array and `strdup`s each name, so
  the source array may be a stack literal in `main`.
- `table_new` stores a **borrowed** `Schema*`; `table_free` frees the page buffers
  but **not** the schema.
- On `.exit`, `do_meta_command` frees the schema and the table (schema first, while
  `table->schema` is still valid).
- Each REPL iteration zero-initializes `Statement statement = {0}` and calls
  `record_free` after execution — a no-op for `select` (NULL payload), the real free
  for `insert`. This plugs what would otherwise be a per-insert leak.

---

## Build system

CMake produces two targets:

```mermaid
flowchart LR
    subgraph lib["mini_sql_lib (static)"]
        a[input_buffer.c]
        b[meta_command.c]
        c[statement.c]
        d[executor.c]
        e[schema.c]
        f[record.c]
        g[table.c]
    end
    main_c[main.c] --> exe["mini_sql (executable)"]
    lib --> exe
    lib -.->|future| tests_bench["tests / bench targets"]
```

Compiled with `-Wall -Wextra -Wpedantic` under strict C11 (`CMAKE_C_EXTENSIONS
OFF`), and emits `compile_commands.json` for clangd. The library/executable split
exists so the test and benchmark targets can link the engine in one line without
recompiling every source.

---

## Testing

Black-box, output-asserting tests in the style of the tutorial's rspec suite, but
implemented as a dependency-free bash script and registered with CTest — one entry
per case, so a failure names itself.

```mermaid
sequenceDiagram
    participant CT as ctest
    participant SH as run_tests.sh
    participant DB as ./mini_sql

    CT->>SH: MINI_SQL_BIN=... run_tests.sh <case>
    SH->>DB: printf "commands…" | mini_sql
    DB-->>SH: raw stdout
    SH->>SH: normalize (strip " (NN.NNN ms)" + trailing ws)
    SH->>SH: assert expected line present
    SH-->>CT: exit 0 (pass) / 1 (fail)
```

The `normalize` step strips the timing suffix so assertions stay deterministic. The
five cases mirror cstack Part 4: round-trip, table-full, max-length strings,
over-length strings, and negative id.

```sh
ctest --test-dir build --output-on-failure
```

---

## Design decisions

**Schema-driven, not struct-driven.** The tutorial's `Row` struct bakes the row
layout into the compiler; `ALTER TABLE` is then impossible without changing C source.
`mini-sql` computes the layout in `schema_create`, so a row is just bytes plus a
ruler. Cost: a layer of indirection now. Payoff: runtime `CREATE TABLE` / `ALTER`
later with no change to `record`, `table`, or `executor`.

**Address columns by stable `column_id`.** Never by name or ordinal. `RENAME` becomes
a metadata edit; `DROP` becomes a flag; neither disturbs other columns' data.

**Validate before allocate.** `prepare_insert` proves the whole line is valid before
calling `record_init`. Early returns can't leak, and there's no half-built record to
unwind.

**`%.*s`, not `%s`, when printing text.** A maximum-width text field has no room for a
NUL terminator. Bounding the print to `column->size` means `mini-sql` never had the
"garbage bytes on max-length strings" bug the tutorial hits — and never needed the
"+1 byte" struct fix, because there is no struct.

**A file-static `default_schema`.** A deliberate stopgap: the parser needs *a* schema
to build records, and there is no catalog yet. When the catalog lands, the parser
will resolve a table by name and this global disappears.

**Known simplification:** the negative-value check applies to all `INT` columns, not
just a primary key — real SQL allows negative integers. This is a placeholder until
`ColumnDefinition` grows real constraints.

---

## Roadmap

Built bottom-up so something runs at every step. Storage-engine priorities
(pager, B-tree) come before query-language breadth.

```mermaid
flowchart LR
    P1["1 · REPL"] --> P2["2 · compiler skeleton"] --> P3["3 · in-memory table"] --> P4["4 · tests"]
    P4 --> P5["5 · pager / persistence"]
    P5 --> P6["6 · cursor abstraction"]
    P6 --> P7["7–13 · B-tree"]
    P7 --> CAT["catalog"]
    CAT --> DDL["CREATE / ALTER TABLE"]

    classDef done fill:#d6f5d6,stroke:#3a9a3a;
    classDef now fill:#fff2cc,stroke:#bba12a;
    classDef todo fill:#eeeeee,stroke:#999999;

    class P1,P2,P3,P4 done;
    class P5 now;
    class P6,P7,CAT,DDL todo;
```

- **Next — Pager (5):** replace `table.c`'s `void* pages[]` with a file-backed page
  cache (`pager_open` / `pager_get_page` / `pager_flush` / `pager_close`). `Table`
  trades its page array for a `Pager*` + root page. First time data survives a restart.
- **Cursor (6):** a `Cursor` abstraction (start/end/advance/value) so insert and
  select stop indexing pages directly — the seam the B-tree slots into.
- **B-tree (7–13):** replace the append-only array with an ordered tree keyed by `id`;
  leaf format → split → internal nodes → multi-level search and scan.
- **Catalog + DDL:** name→table registry, then runtime `CREATE TABLE` and the
  `ALTER TABLE ADD/DROP/RENAME COLUMN` family the schema layer was designed for.

---

## Project layout

```text
mini-sql/
├── CMakeLists.txt          # two targets: mini_sql_lib (static) + mini_sql (exe)
├── include/
│   ├── input_buffer.h      # line reader
│   ├── meta_command.h      # dot-commands
│   ├── schema.h            # ColumnType, ColumnDefinition, Schema
│   ├── record.h            # Record + (de)serialization
│   ├── table.h             # Table + paging
│   ├── statement.h         # Statement, PrepareResult
│   └── executor.h          # ExecuteResult, execute_statement
├── src/
│   ├── input_buffer.c
│   ├── meta_command.c
│   ├── schema.c
│   ├── record.c
│   ├── table.c
│   ├── statement.c
│   ├── executor.c
│   └── main.c              # REPL — the only file with no header
└── tests/
    └── run_tests.sh        # black-box suite, driven by CTest
```

---

## Limitations & non-goals

This is a learning engine. It is **not** trying to be:

- **Persistent** — everything lives in process memory; `.exit` discards it. (Until Part 5.)
- **A real SQL dialect** — `insert`/`select` only, fixed positional syntax, one
  hardcoded `users` table, no `WHERE` / `UPDATE` / `DELETE` / joins.
- **A bytecode VM** — the executor runs statements directly; there is no parse tree,
  no code generator, no VDBE.
- **Concurrent or durable** — single-threaded, no locking, no WAL, no transactions.

Each of those is a known gap with a place on the [roadmap](#roadmap), not an
accident. The aim is a correct, legible core that grows one well-understood layer at
a time.

---

*Built by following cstack's tutorial, with a schema-driven spine bolted in from the
start.*
