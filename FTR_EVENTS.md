# FTR Transaction Events Convention

This document defines a convention for recording events inside FTR transactions
using only existing FTR primitives (transactions and relations), and shows how
to write such traces with the current `ftr::ftr_writer` API.

[Minres/LWTR4SC issue #8](https://github.com/Minres/LWTR4SC/issues/8) proposed
first-class event support in the FTR format itself: new CBOR tags inside the
transaction block (tag 18 for an event without attributes, tag 19 for an event
with attributes), modeled after OpenTelemetry span events. Upstream instead
resolved the issue with
[PR #9](https://github.com/Minres/LWTR4SC/pull/9), which implements events on
top of existing FTR primitives through a `record_event` API in the LWTR4SC
SystemC library — the same approach specified here. This document pins down
that convention precisely so that standalone writers (such as
`ftr::ftr_writer`) and viewers interoperate with traces produced by the
reference implementation. First-class tags would still be the more compact
encoding (see
[Comparison with first-class events](#comparison-with-the-first-class-cbor-events-proposal))
but they require format, writer, and parser changes.

The key words MUST, SHOULD, and MAY are to be interpreted as in RFC 2119.

## Problem

FTR transactions already have a start time, an end time, and transaction
attributes. That is enough to describe an operation with duration, such as an
instruction, bus access, or packet transfer.

Performance simulators also need to record meaningful points or short intervals
inside a transaction:

- instruction stalls with a reason and resource id
- operand fetches with operand id and register bank
- NoC packet hops with node coordinates
- backpressure, arbitration, retry, or hazard events

Without a convention, writers can only encode these as ordinary transaction
attributes, for example:

```text
events.l2_hit.timestamp = 123
events.l2_hit.node_id = 4
events.l2_hit.clid = 3
```

That preserves some data, but it does not give viewers a standard structure to
render. A GUI cannot reliably know which attributes are events, how they should
be ordered, where they sit inside the parent transaction, or which attributes
belong to the same event.

## Convention

1. A normal transaction is written through a parent generator, for example
   `instruction`.
2. Events for a parent generator MUST be written through a generator in the
   same stream named `<parent_generator_name>.events`, for example
   `instruction.events`. (The LWTR4SC reference implementation creates this
   generator automatically when a `tx_generator` is constructed with
   `with_events = true`.)
3. Each event MUST be represented as one transaction from the `.events`
   generator.
4. Each event transaction MUST be linked to exactly one parent transaction by
   a relation named `parent_of`, with the parent transaction as the relation
   source and the event transaction as the relation sink. Note that the
   relation name is not an event discriminator: the reference implementation
   uses the same `parent_of` relation for ordinary parent/child links between
   regular transactions.
5. Writers MUST emit the 5-element relation form that includes both stream
   ids (this is what `ftr_writer::writeRelation` produces).
6. Each event transaction MUST carry a `BEGIN` attribute named `name` of
   type `STRING` holding the event name. The LWTR4SC reference implementation
   records it: the `.events` generator declares `name` as its begin attribute,
   and `record_event` writes the event name through it. Additional event
   metadata SHOULD be attached as further attributes on the event
   transaction, not as dotted attributes on the parent transaction.
7. The event transaction time range SHOULD lie inside the parent transaction
   time range. Zero-duration event transactions are allowed and are the
   normal representation for point-in-time annotations (the reference
   `record_event` always produces zero-duration events).
8. Event transactions with the same parent MAY overlap in time (for example,
   two concurrent stalls on different resources). Viewers already assign
   overlapping transactions of one generator to separate display lanes, so
   overlap needs no special restriction.

### Identifying events and their parents

A transaction is an event if and only if it was written through a `.events`
generator: generator membership is the discriminator. The relation supplies
the parent: viewers MUST resolve parentage through the event transaction's
incoming `parent_of` relation (parent = source). The relation name alone MUST
NOT be used to identify events, because reference traces use `parent_of` for
generic transaction hierarchy as well.

When the signals are incomplete or inconsistent:

- A transaction in a `.events` generator with no incoming `parent_of`
  relation has no parent; viewers SHOULD render it as an ordinary transaction
  of that generator.
- A transaction in a `.events` generator with multiple incoming `parent_of`
  relations is malformed; viewers SHOULD use one parent (for example the
  first) and MAY flag the trace.
- A `parent_of` relation whose sink is not in a `.events` generator is an
  ordinary hierarchy relation, not an event link.

### Violation handling

Writers cannot always validate rule 7 at emit time (the parent's end time may
not be known yet when an event is written), so viewers will encounter events
outside their parent's range. Viewers MUST NOT discard such events; they
SHOULD render them at their recorded time and MAY visually flag them as out of
range.

### Rendering

Viewers can use the `.events` suffix as an opt-in signal for specialized
rendering. For example, a timeline can render event markers on top of the
parent transaction bar and show a separate event table for the selected parent
transaction.
Viewers that do not implement special handling still degrade gracefully:
because events live in their own generator, a per-generator view shows them as
a parallel row directly below the parent generator, and relation-aware viewers
(such as Surfer) already highlight the parent/event link when a transaction is
focused.

## LWTR4SC API Usage (Reference Implementation)

Since PR #9, the LWTR4SC SystemC library implements this convention directly.
Constructing a `tx_generator` with `with_events = true` automatically creates
the `<name>.events` generator in the same fiber and the `parent_of` relation;
`tx_handle::record_event` (current simulation time) and
`tx_handle::record_event_at_time` (explicit timestamp) then write one
zero-duration event transaction each, linked to the parent, with the event
name recorded as the `BEGIN` attribute `name` per rule 6. The code below
runs inside a SystemC process (an `SC_THREAD`), with the database set up in
`sc_main`:

```cpp
#include <lwtr/lwtr.h>

// In sc_main, before sc_start():
//   lwtr::tx_ftr_init(true);            // FTR backend, LZ4-compressed
//   lwtr::tx_db db("instruction_trace");
//   lwtr::tx_db::set_default_db(&db);

void record_instruction(lwtr::tx_fiber& cpu_fiber) {
    using namespace sc_core;

    // with_events = true creates the "instruction.events" generator and the
    // parent_of relation used by record_event.
    static lwtr::tx_generator<uint64_t, char const*> instruction_gen(
        "instruction", cpu_fiber, "pc", "result", true);

    auto insn_tx = instruction_gen.begin_tx(uint64_t{0x80000000});
    insn_tx.record_attribute("opcode", "LW");

    // Event at the current simulation time, with attributes:
    insn_tx.record_event("operand_fetch",
                         "operand", uint64_t{0},
                         "register_bank", uint64_t{2});

    // Event at an explicit timestamp inside the transaction:
    insn_tx.record_event_at_time("stall",
                                 sc_time_stamp() + sc_time(2, SC_NS),
                                 "reason", "data_hazard",
                                 "cycles", uint64_t{3});

    insn_tx.end_tx_delayed<char const*>(sc_time_stamp() + sc_time(3, SC_NS),
                                        "ok");
}
```

One caveat with the current upstream implementation: events are always
zero-duration. `record_event_at_time` begins the event at the given timestamp
and ends it immediately (end times earlier than the begin time are clamped to
the begin time).

## Standalone ftr_writer Usage

The standalone writer in this repository does not provide a dedicated
`record_event` helper. Use the primitive APIs directly:

- `writeStream` declares the transaction stream.
- `writeGenerator` declares the parent generator and the `.events` generator.
- `startTransaction` and `endTransaction` write the parent and event
  transactions.
- `writeAttribute` attaches structured event metadata to the event transaction.
- `writeRelation` links the parent transaction to the event transaction.

`writeRelation` takes sink first, then source:

```cpp
writer.writeRelation(name, sink_stream_id, sink_tx_id, source_stream_id, source_tx_id);
```

The following example writes one instruction transaction with two events:

```cpp
#include <ftr/ftr_writer.h>

#include <cstdint>
#include <string>

void write_instruction_trace(const std::string& output_path) {
    ftr::ftr_writer<> writer(output_path);

    writer.writeInfo(-12); // ps

    constexpr std::uint64_t cpu_stream = 1;
    constexpr std::uint64_t instruction_gen = 10;
    constexpr std::uint64_t instruction_events_gen = 11;

    writer.writeStream(cpu_stream, "cpu0", "transaction_stream");
    writer.writeGenerator(instruction_gen, "instruction", cpu_stream);
    writer.writeGenerator(instruction_events_gen, "instruction.events", cpu_stream);

    constexpr std::uint64_t insn_tx = 1000;
    writer.startTransaction(insn_tx, instruction_gen, cpu_stream, 100);
    writer.writeAttribute(insn_tx, ftr::event_type::BEGIN, "pc",
                          ftr::data_type::UNSIGNED, std::uint64_t{0x80000000});
    writer.writeAttribute(insn_tx, ftr::event_type::RECORD, "opcode",
                          ftr::data_type::STRING, "LW");

    constexpr std::uint64_t fetch_event_tx = 2000;
    writer.startTransaction(fetch_event_tx, instruction_events_gen, cpu_stream, 108);
    writer.writeAttribute(fetch_event_tx, ftr::event_type::BEGIN, "name",
                          ftr::data_type::STRING, "operand_fetch");
    writer.writeAttribute(fetch_event_tx, ftr::event_type::RECORD, "operand",
                          ftr::data_type::UNSIGNED, std::uint64_t{0});
    writer.writeAttribute(fetch_event_tx, ftr::event_type::RECORD, "register_bank",
                          ftr::data_type::UNSIGNED, std::uint64_t{2});
    writer.endTransaction(fetch_event_tx, 108);
    writer.writeRelation("parent_of", cpu_stream, fetch_event_tx, cpu_stream, insn_tx);

    constexpr std::uint64_t stall_event_tx = 2001;
    writer.startTransaction(stall_event_tx, instruction_events_gen, cpu_stream, 112);
    writer.writeAttribute(stall_event_tx, ftr::event_type::BEGIN, "name",
                          ftr::data_type::STRING, "stall");
    writer.writeAttribute(stall_event_tx, ftr::event_type::RECORD, "reason",
                          ftr::data_type::STRING, "data_hazard");
    writer.writeAttribute(stall_event_tx, ftr::event_type::RECORD, "cycles",
                          ftr::data_type::UNSIGNED, std::uint64_t{3});
    writer.endTransaction(stall_event_tx, 115);
    writer.writeRelation("parent_of", cpu_stream, stall_event_tx, cpu_stream, insn_tx);

    writer.writeAttribute(insn_tx, ftr::event_type::END, "retired",
                          ftr::data_type::BOOLEAN, true);
    writer.endTransaction(insn_tx, 130);
}
```

## Suggested Helper

Applications can wrap the primitive calls in a small helper so call sites read
like event recording rather than generic transaction writing:

```cpp
void record_zero_duration_event(ftr::ftr_writer<>& writer,
                                std::uint64_t stream_id,
                                std::uint64_t parent_tx_id,
                                std::uint64_t event_tx_id,
                                std::uint64_t event_generator_id,
                                std::uint64_t timestamp,
                                const char* event_name) {
    writer.startTransaction(event_tx_id, event_generator_id, stream_id, timestamp);
    writer.writeAttribute(event_tx_id, ftr::event_type::BEGIN, "name",
                          ftr::data_type::STRING, event_name);
    writer.endTransaction(event_tx_id, timestamp);
    writer.writeRelation("parent_of", stream_id, event_tx_id, stream_id, parent_tx_id);
}
```

Example call:

```cpp
record_zero_duration_event(writer, cpu_stream, insn_tx, 2002,
                           instruction_events_gen, 120,
                           "writeback");
```

For events with attributes, keep the event transaction open long enough to write
the attributes, then close it and add the relation:

```cpp
writer.startTransaction(2003, instruction_events_gen, cpu_stream, 122);
writer.writeAttribute(2003, ftr::event_type::BEGIN, "name",
                      ftr::data_type::STRING, "register_bank_conflict");
writer.writeAttribute(2003, ftr::event_type::RECORD, "bank_id",
                      ftr::data_type::UNSIGNED, std::uint64_t{1});
writer.writeAttribute(2003, ftr::event_type::RECORD, "operand_addr",
                      ftr::data_type::UNSIGNED, std::uint64_t{5});
writer.endTransaction(2003, 122);
writer.writeRelation("parent_of", cpu_stream, 2003, cpu_stream, insn_tx);
```

## Writer Checklist

- Name the event generator exactly `<parent_generator_name>.events`.
- Put the event generator in the same stream as the parent generator.
- Link every event transaction to its parent transaction with a relation
  named `parent_of` (parent = source, event = sink).
- Give every event transaction a `BEGIN` attribute `name` of type `STRING`
  (required by rule 6).
- Keep event timestamps within the parent transaction range.
- Use zero-duration events for point-in-time annotations and short-duration
  events when the event has a meaningful duration.
- Store event-specific metadata as attributes on the event transaction, not as
  dotted attributes on the parent transaction.
- Allocate a unique transaction id for every event (event transactions share
  the id space with all other transactions).

## Comparison with the First-Class CBOR Events Proposal

Issue #8 originally proposed encoding an event as `(timestamp, name)` (tag 18)
or `(timestamp, name, attributes)` (tag 19) inside the parent transaction's
element list. That encoding was not adopted — merged PR #9 implements the
transaction-plus-relation convention in this document instead — but the cost
comparison remains useful. A tag-encoded event inherits the parent's stream,
generator, and identity, so it needs no transaction id, no end time, and no
relation.

Approximate uncompressed sizes with 64-bit picosecond timestamps,
multi-million transaction ids, and dictionary-interned strings:

| Encoding | Bare event | Event with 2 attributes |
|---|---|---|
| This convention (transaction + relation) | ~47 bytes | ~60 bytes |
| First-class tag 18/19 | ~12 bytes | ~27 bytes |

So the convention costs roughly 4x for bare events, narrowing to roughly 2x as
payload attributes are added; LZ4 chunk compression narrows the on-disk gap
further. The writer also pays per-event transaction-id allocation and open-
transaction bookkeeping, and readers pay per-event transaction and relation
records (see the next section for the reader-side fixes this requires).

The convention's compensating advantages: it works today with unmodified
writers, parsers, and viewers; events are ordinary transactions, so every
existing feature (attributes, relations, lazy stream loading, rendering)
applies to them unchanged. Should first-class tags be adopted later, the
migration is mechanical: each event transaction maps one-to-one onto a tag 19
record inside its parent.

## Required Ecosystem Fixes and Improvements

The convention is viable at performance-simulator scale (millions of events)
only with the following fixes. References are to `ftr_parser` 0.3.0 and
current Surfer sources.

### ftr_parser: index relations by transaction id (required)

Relation attachment is quadratic today. Each transaction parsed in
`parse_tx_block` linearly scans the entire relation list
(`ftr_parser.rs:442`), and the byte-buffer path repeats the same scan as a
triple loop in `connect_relations_and_transactions` (`ftr_parser.rs:634`).
Under this convention the number of relations is roughly equal to the number
of transactions, so a 1M-event trace performs on the order of 10^12
comparisons and is effectively unloadable.

Fix: when relation chunks are parsed, build two hash maps from transaction id
to relation indices (one keyed by source, one by sink), and replace both scan
sites with lookups. Load complexity drops from O(transactions x relations) to
O(transactions + relations). No public API change.

### ftr_parser: store each relation once (recommended)

Each `TxRelation` carries a heap-allocated name `String` and is cloned into
both the parent's `out_relations` and the event's `inc_relations`
(`types.rs:104`). Storing indices into the single `tx_relations` vector (or at
least interning the relation name) roughly halves relation memory. Minor API
change for consumers.

### ftr_parser: use u64 timestamps (recommended)

`Event` stores `start_time`/`end_time` as `BigUint`, and the accessors clone
them on every call (`types.rs:124`). Surfer's render loop binary-searches
transactions keyed by `get_end_time`, paying a heap clone per comparison per
frame. The FTR writer API only emits `uint64_t` times, so `u64` loses nothing
and removes per-event heap allocations on the hottest rendering path.

### ftr_parser: intern attribute names and string values (recommended)

Every attribute materializes fresh `String`s for its name and string value
(`types.rs:157`), so 1M `stall` events hold 1M copies of `"name"` and
`"stall"` that the on-disk dictionary had already deduplicated. Keeping
dictionary ids (or `Arc<str>`) restores that sharing in memory.

### ftr_parser: fix the 3-element relation fallback (bug, unrelated path)

For relations without explicit stream ids, both stream lookups search by the
source transaction id (`ftr_parser.rs:484`), so the sink stream id is wrong,
and each lookup is a full scan over all loaded transactions. This convention
mandates 5-element relations (rule 5) and never hits this path, but the
fallback should be fixed upstream regardless.

### Surfer: opt-in event rendering (enhancement)

Surfer already renders the convention acceptably with no changes: a `.events`
generator added as its own row shows events on per-generator lanes, and
focusing a transaction highlights its relations. Dedicated support would add:

- event markers drawn on top of the parent generator's transaction bars,
  driven by the `.events` suffix plus incoming `parent_of` relations
- an event table for the currently focused parent transaction
- awareness in whole-stream view, which currently draws all generators of a
  stream into one displayed item using per-generator lane indices, so a parent
  and a concurrent event can be drawn on top of each other (a pre-existing
  property of whole-stream rendering for any multi-generator stream)

### Standalone ftr_writer: record_event helper (enhancement)

The standalone `ftr::ftr_writer` has no event helper; a `record_event`
mirroring the [Suggested Helper](#suggested-helper) would make the convention
easy to adopt from non-SystemC writers.
