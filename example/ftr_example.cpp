/**
 * @file ftr_example.cpp
 * @brief Demonstrates FTR (Fast Transaction Recording) trace generation for a
 *        dual-issue RISC-V CPU pipeline with memory transactions.
 *
 * This example shows how to use the low-level ftr_writer API to record
 * transaction-level activity in a simulated processor model. It demonstrates:
 *
 * ## Key Concepts
 *
 * ### Events as Zero-Duration Transactions
 * Events are transactions with identical start and end times (duration = 0).
 * This is useful for modeling discrete occurrences like pipeline stages or
 * bus phases (BEGIN_REQ, END_REQ, etc.). Each event is recorded as a child
 * transaction linked to its parent via a "parent_of" relation.
 *
 * ### Transaction Relations
 * Relations link transactions across different streams/fibers. This example
 * uses "parent_of" relations to:
 * - Link instruction transactions to their pipeline stage events
 * - Link load/store instructions to their memory bus transactions
 * - Link memory transactions to their fabric progression events
 *
 * This enables tracing a memory access at a peripheral back to the originating
 * CPU instruction, similar to how RISC-V VP links instructions to bus
 * transactions.
 *
 * ### Dual-Issue Pipeline Model
 * The example models a dual-issue CPU that can dispatch 2 instructions per
 * cycle. Each instruction progresses through 5 pipeline stages:
 * - IF  (Instruction Fetch)
 * - ID  (Instruction Decode)
 * - EX  (Execute)
 * - MEM (Memory Access)
 * - WB  (Write Back)
 *
 * ### Memory Fabric Model
 * Load/store instructions generate child transactions on the Memory fiber.
 * Each memory transaction progresses through 5 fabric stages:
 * - REQ    (Request initiation)
 * - ARB    (Arbitration)
 * - ROUTE  (Interconnect routing)
 * - ACCESS (Memory/peripheral access)
 * - RESP   (Response delivery)
 *
 * ## Trace Structure
 *
 * The generated trace contains:
 * - 2 Fibers (streams):
 *   - CPU_Core: instruction transactions with pipeline stage events
 *   - Memory: bus transactions with fabric progression events
 * - 4 Generators:
 *   - CPU_Core_tx: main instruction transactions
 *   - events (on CPU_Core): pipeline stage events
 *   - Memory_tx: memory access transactions
 *   - events (on Memory): fabric stage events
 *
 * ## Transaction Class API
 *
 * The Transaction class wraps ftr_writer providing:
 * - Constructor: starts a transaction on a fiber
 * - add_attribute(): records named attributes (string, uint64, int64, bool)
 * - record_event(): creates zero-duration child transaction with attributes
 * - add_child_transaction(): creates child transaction on another fiber
 * - end(): closes the transaction with an end time
 *
 * ## Usage
 *
 * Build and run:
 * @code
 *   cmake --build build --target ftr_example
 *   ./build/example/ftr_example
 * @endcode
 *
 * Output: riscv_pipeline.ftr (CBOR-encoded binary trace)
 *
 * @see ftr_writer.h for the underlying FTR format specification
 */

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ftr/ftr_writer.h"

// Forward declaration
template <bool COMPRESSED>
class Transaction;

// Fiber with transaction and event generators
template <bool COMPRESSED>
struct Fiber {
    uint64_t fiber_id;
    uint64_t tx_generator_id;
    uint64_t event_generator_id;
    ftr::ftr_writer<COMPRESSED>* writer;

    Fiber(ftr::ftr_writer<COMPRESSED>* w, uint64_t fid, const std::string& name,
          const std::string& kind, uint64_t tx_gen_id, uint64_t event_gen_id)
        : fiber_id(fid)
        , tx_generator_id(tx_gen_id)
        , event_generator_id(event_gen_id)
        , writer(w) {
        writer->writeStream(fiber_id, name, kind);
        writer->writeGenerator(tx_generator_id, name + "_tx", fiber_id);
        writer->writeGenerator(event_generator_id, "events", fiber_id);
    }
};

// Transaction class wrapping FTR writer functionality
template <bool COMPRESSED>
class Transaction {
    ftr::ftr_writer<COMPRESSED>* writer_;
    uint64_t tx_id_;
    uint64_t fiber_id_;
    uint64_t event_generator_id_;
    uint64_t start_time_;
    bool ended_;

    // Counter for generating unique event IDs
    static uint64_t next_event_id_;

public:
    Transaction(ftr::ftr_writer<COMPRESSED>* writer, const Fiber<COMPRESSED>& fiber,
                uint64_t tx_id, uint64_t start_time)
        : writer_(writer)
        , tx_id_(tx_id)
        , fiber_id_(fiber.fiber_id)
        , event_generator_id_(fiber.event_generator_id)
        , start_time_(start_time)
        , ended_(false) {
        writer_->startTransaction(tx_id_, fiber.tx_generator_id, fiber_id_, start_time_);
    }

    ~Transaction() {
        if (!ended_) {
            end(start_time_);
        }
    }

    // Non-copyable
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    // Movable
    Transaction(Transaction&& other) noexcept
        : writer_(other.writer_)
        , tx_id_(other.tx_id_)
        , fiber_id_(other.fiber_id_)
        , event_generator_id_(other.event_generator_id_)
        , start_time_(other.start_time_)
        , ended_(other.ended_) {
        other.ended_ = true;  // Prevent double-end
    }

    uint64_t get_id() const { return tx_id_; }
    uint64_t get_fiber_id() const { return fiber_id_; }

    // Add attribute to transaction
    template <typename T>
    void add_attribute(const char* name, ftr::data_type type, T value,
                       ftr::event_type event = ftr::event_type::RECORD) {
        writer_->writeAttribute(tx_id_, event, name, type, value);
    }

    void add_attribute(const char* name, const char* value,
                       ftr::event_type event = ftr::event_type::RECORD) {
        writer_->writeAttribute(tx_id_, event, name, ftr::data_type::STRING, value);
    }

    void add_attribute(const char* name, uint64_t value,
                       ftr::event_type event = ftr::event_type::RECORD) {
        writer_->writeAttribute(tx_id_, event, name, ftr::data_type::UNSIGNED, value);
    }

    void add_attribute(const char* name, int64_t value,
                       ftr::event_type event = ftr::event_type::RECORD) {
        writer_->writeAttribute(tx_id_, event, name, ftr::data_type::INTEGER, value);
    }

    void add_attribute(const char* name, bool value,
                       ftr::event_type event = ftr::event_type::RECORD) {
        writer_->writeAttribute(tx_id_, event, name, ftr::data_type::BOOLEAN, value);
    }

    // Record an event (zero-duration child transaction)
    template <typename... NameValues>
    uint64_t record_event(const char* name, uint64_t timestamp, NameValues&&... nvs) {
        uint64_t event_id = next_event_id_++;
        writer_->startTransaction(event_id, event_generator_id_, fiber_id_, timestamp);
        writer_->writeAttribute(event_id, ftr::event_type::BEGIN, "event", ftr::data_type::STRING, name);
        record_event_attrs(event_id, std::forward<NameValues>(nvs)...);
        writer_->endTransaction(event_id, timestamp);  // Same start/end = zero duration
        // Add relation: this transaction is parent of the event
        writer_->writeRelation("parent_of", fiber_id_, event_id, fiber_id_, tx_id_);
        return event_id;
    }

    // Add a child transaction on a different fiber
    Transaction<COMPRESSED> add_child_transaction(const Fiber<COMPRESSED>& child_fiber,
                                                   uint64_t child_tx_id,
                                                   uint64_t start_time) {
        Transaction<COMPRESSED> child(writer_, child_fiber, child_tx_id, start_time);
        // Add relation: this transaction is parent of child
        writer_->writeRelation("parent_of", child_fiber.fiber_id, child_tx_id, fiber_id_, tx_id_);
        return child;
    }

    void end(uint64_t end_time) {
        if (!ended_) {
            writer_->endTransaction(tx_id_, end_time);
            ended_ = true;
        }
    }

private:
    // Base case for variadic template
    void record_event_attrs(uint64_t) {}

    // Recursive case: process pairs of (name, value)
    template <typename T, typename... Rest>
    void record_event_attrs(uint64_t event_id, const char* attr_name, T value, Rest&&... rest) {
        write_attr(event_id, attr_name, value);
        record_event_attrs(event_id, std::forward<Rest>(rest)...);
    }

    void write_attr(uint64_t event_id, const char* name, const char* value) {
        writer_->writeAttribute(event_id, ftr::event_type::RECORD, name, ftr::data_type::STRING, value);
    }

    void write_attr(uint64_t event_id, const char* name, uint64_t value) {
        writer_->writeAttribute(event_id, ftr::event_type::RECORD, name, ftr::data_type::UNSIGNED, value);
    }

    void write_attr(uint64_t event_id, const char* name, int64_t value) {
        writer_->writeAttribute(event_id, ftr::event_type::RECORD, name, ftr::data_type::INTEGER, value);
    }

    void write_attr(uint64_t event_id, const char* name, uint32_t value) {
        writer_->writeAttribute(event_id, ftr::event_type::RECORD, name, ftr::data_type::UNSIGNED, static_cast<uint64_t>(value));
    }

    void write_attr(uint64_t event_id, const char* name, bool value) {
        writer_->writeAttribute(event_id, ftr::event_type::RECORD, name, ftr::data_type::BOOLEAN, value);
    }
};

template <bool COMPRESSED>
uint64_t Transaction<COMPRESSED>::next_event_id_ = 10000;  // Start event IDs high to avoid collision

// Pipeline stages for RISC-V
enum class PipelineStage { FETCH, DECODE, EXECUTE, MEMORY, WRITEBACK };

const char* stage_name(PipelineStage stage) {
    switch (stage) {
        case PipelineStage::FETCH:     return "IF";
        case PipelineStage::DECODE:    return "ID";
        case PipelineStage::EXECUTE:   return "EX";
        case PipelineStage::MEMORY:    return "MEM";
        case PipelineStage::WRITEBACK: return "WB";
    }
    return "UNKNOWN";
}

// Memory fabric stages
enum class MemoryStage { REQUEST, ARBITRATE, ROUTE, ACCESS, RESPONSE };

const char* mem_stage_name(MemoryStage stage) {
    switch (stage) {
        case MemoryStage::REQUEST:   return "REQ";
        case MemoryStage::ARBITRATE: return "ARB";
        case MemoryStage::ROUTE:     return "ROUTE";
        case MemoryStage::ACCESS:    return "ACCESS";
        case MemoryStage::RESPONSE:  return "RESP";
    }
    return "UNKNOWN";
}

int main() {
    ftr::ftr_writer<false> writer("riscv_pipeline.ftr");

    writer.writeInfo(-9); // nanosecond time base

    // Create fibers: CPU Core and Memory
    // Fiber IDs: 0 = CPU Core, 1 = Memory
    // Generator IDs: 0 = cpu_tx, 1 = cpu_events, 2 = mem_tx, 3 = mem_events
    Fiber<false> cpu_fiber(&writer, 0, "CPU_Core", "instructions", 0, 1);
    Fiber<false> mem_fiber(&writer, 1, "Memory", "bus_transactions", 2, 3);

    uint64_t tx_id = 1;
    uint64_t cycle = 0;
    const uint64_t CYCLE_TIME = 10;  // 10 ns per cycle
    const uint64_t STAGE_LATENCY = 1;  // 1 cycle per pipeline stage

    // Simulate dual-issue CPU executing instructions
    struct InstructionInfo {
        const char* mnemonic;
        const char* type;
        uint64_t addr;
        bool is_memory_op;
        uint64_t mem_addr;
        const char* mem_type;
    };

    std::vector<InstructionInfo> program = {
        {"ADD",   "R-type", 0x1000, false, 0, nullptr},
        {"SUB",   "R-type", 0x1004, false, 0, nullptr},
        {"LW",    "I-type", 0x1008, true,  0x2000, "load"},
        {"ADDI",  "I-type", 0x100C, false, 0, nullptr},
        {"SW",    "S-type", 0x1010, true,  0x2004, "store"},
        {"BEQ",   "B-type", 0x1014, false, 0, nullptr},
        {"LW",    "I-type", 0x1018, true,  0x2008, "load"},
        {"AND",   "R-type", 0x101C, false, 0, nullptr},
        {"OR",    "R-type", 0x1020, false, 0, nullptr},
        {"SW",    "S-type", 0x1024, true,  0x200C, "store"},
    };

    // Process instructions in pairs (dual issue)
    for (size_t i = 0; i < program.size(); i += 2) {
        uint64_t issue_time = cycle * CYCLE_TIME;

        // Issue up to 2 instructions simultaneously
        for (size_t j = i; j < std::min(i + 2, program.size()); ++j) {
            const auto& insn = program[j];
            uint64_t insn_tx_id = tx_id++;

            // Create instruction transaction
            Transaction<false> insn_tx(&writer, cpu_fiber, insn_tx_id, issue_time);
            insn_tx.add_attribute("mnemonic", insn.mnemonic, ftr::event_type::BEGIN);
            insn_tx.add_attribute("type", insn.type);
            insn_tx.add_attribute("pc", insn.addr);
            insn_tx.add_attribute("issue_slot", static_cast<uint64_t>(j - i));

            // Record pipeline stage events
            uint64_t stage_time = issue_time;
            for (int s = 0; s < 5; ++s) {
                PipelineStage stage = static_cast<PipelineStage>(s);
                insn_tx.record_event(stage_name(stage), stage_time,
                                     "stage_id", static_cast<uint64_t>(s),
                                     "pc", insn.addr);
                stage_time += STAGE_LATENCY * CYCLE_TIME;
            }

            // If it's a memory operation, create child memory transaction
            if (insn.is_memory_op) {
                uint64_t mem_tx_id = tx_id++;
                uint64_t mem_start = issue_time + 3 * STAGE_LATENCY * CYCLE_TIME;  // MEM stage

                // Create memory transaction as child
                auto mem_tx = insn_tx.add_child_transaction(mem_fiber, mem_tx_id, mem_start);
                mem_tx.add_attribute("op", insn.mem_type, ftr::event_type::BEGIN);
                mem_tx.add_attribute("addr", insn.mem_addr);
                mem_tx.add_attribute("size", static_cast<uint64_t>(4));

                // Record memory fabric progression events
                uint64_t mem_stage_time = mem_start;
                for (int s = 0; s < 5; ++s) {
                    MemoryStage stage = static_cast<MemoryStage>(s);
                    mem_tx.record_event(mem_stage_name(stage), mem_stage_time,
                                        "fabric_node", static_cast<uint64_t>(s),
                                        "addr", insn.mem_addr);
                    mem_stage_time += 2 * CYCLE_TIME;  // Memory fabric has 2-cycle latency per stage
                }

                // Memory transaction ends after all fabric stages
                mem_tx.end(mem_stage_time);

                // Instruction takes longer due to memory access
                insn_tx.add_attribute("result", "ok", ftr::event_type::END);
                insn_tx.end(mem_stage_time);
            } else {
                // Regular instruction ends after pipeline
                insn_tx.add_attribute("result", "ok", ftr::event_type::END);
                insn_tx.end(stage_time);
            }
        }

        // Advance cycle for next pair
        cycle += 5;  // Basic pipeline throughput
    }

    return 0;
}
