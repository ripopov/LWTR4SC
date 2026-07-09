/*******************************************************************************
 * konata2ftr - convert a Konata/Kanata pipeline log into an FTR trace.
 *
 * Input:  Kanata log (tab separated, see Konata's docs/kanata-log-format.md),
 *         read from a file argument or stdin (pipe gzipped logs through zcat).
 * Output: LZ4-compressed FTR file following the transaction-events convention
 *         from FTR_EVENTS.md:
 *           - each instruction is a transaction of the "instruction" generator
 *           - each pipeline stage is a transaction of the "instruction.events"
 *             generator, linked to its instruction by a "parent_of" relation
 *             (event name = stage name, "lane" attribute = Kanata lane id)
 *
 * Command mapping:
 *   C= / C     absolute / relative cycle. 1 cycle = 1 FTR time unit.
 *   I          begin instruction transaction; BEGIN attributes
 *              "insn_id_in_sim" and "thread_id".
 *   L type 0   RECORD attribute "label" on the instruction (PC + disasm)
 *   L type 1   RECORD attribute "detail" on the instruction
 *   L type 2   RECORD attribute "label" on the currently open stage event
 *              (falls back to "detail" on the instruction if none is open)
 *   S          begin stage event; an open stage on the same lane is closed
 *              first, mirroring Konata's implicit stage end
 *   E          end stage event and link it to the instruction
 *   R          close remaining stages, END attributes "retire_id"/"flushed";
 *              the transaction ends at this cycle (written out at EOF so
 *              late labels for retired instructions still attach)
 *   W          "wakeup" relation from producer to consumer instruction
 *   EOF        instructions the log never retired are closed at the last cycle
 *
 * Usage: konata2ftr <output.ftr> [input.log]
 *        zcat kanata-sample-2.log.gz | konata2ftr kanata-sample-2.ftr
 ******************************************************************************/

#include <ftr/ftr_writer.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint64_t STREAM_ID = 1;
constexpr uint64_t GEN_INSTRUCTION = 10;
constexpr uint64_t GEN_INSTRUCTION_EVENTS = 11;
// Stage events get transaction ids from a range far above any instruction id
// (instruction tx id = Kanata file id + 1).
constexpr uint64_t EVENT_TX_BASE = 1000000;

struct Instruction {
    uint64_t tx_id = 0;
    // lane id -> open stage event transaction id
    std::unordered_map<uint64_t, uint64_t> open_stage_by_lane;
    // last started stage event (target of "L type 2" labels) and whether it
    // is still open for attribute writes
    uint64_t last_stage_tx = 0;
    bool last_stage_open = false;
    // R has been seen. Konata still accepts labels for retired instructions
    // (the sample logs emit the disassembly right after the flush R), so the
    // FTR transaction is only ended at EOF, at the recorded retire time.
    bool retired = false;
    uint64_t retire_time = 0;
};

struct Stats {
    uint64_t instructions = 0;
    uint64_t stages = 0;
    uint64_t retires = 0;
    uint64_t flushes = 0;
    uint64_t wakeups = 0;
    uint64_t labels = 0;
    uint64_t unmatched_stage_ends = 0;
    uint64_t stage_labels_on_closed_stage = 0;
    uint64_t unretired_at_eof = 0;
};

class Converter {
public:
    explicit Converter(const std::string& output_path)
    : writer(output_path) {
        writer.writeInfo(-12);
        writer.writeStream(STREAM_ID, "pipeline", "transaction_stream");
        writer.writeGenerator(GEN_INSTRUCTION, "instruction", STREAM_ID);
        writer.writeGenerator(GEN_INSTRUCTION_EVENTS, "instruction.events", STREAM_ID);
    }

    void process_line(const std::string& line, size_t line_no) {
        if(line.empty())
            return;
        auto fields = split_tabs(line);
        const std::string& cmd = fields[0];
        try {
            if(cmd == "Kanata")
                return; // header, any version
            else if(cmd == "C=")
                cur_cycle = to_i64(fields.at(1));
            else if(cmd == "C")
                cur_cycle += to_i64(fields.at(1));
            else if(cmd == "I")
                cmd_begin_instruction(fields);
            else if(cmd == "L")
                cmd_label(fields, line);
            else if(cmd == "S")
                cmd_stage_begin(fields);
            else if(cmd == "E")
                cmd_stage_end(fields);
            else if(cmd == "R")
                cmd_retire(fields);
            else if(cmd == "W")
                cmd_wakeup(fields);
            else
                throw std::runtime_error("unknown command '" + cmd + "'");
        } catch(std::exception& e) {
            throw std::runtime_error("line " + std::to_string(line_no) + ": " + e.what());
        }
    }

    void finish() {
        // End every instruction transaction: retired ones at their recorded
        // retire time, ones the log left open at its final cycle. Ending in
        // ascending (end time, tx id) order keeps the transactions sorted by
        // end time inside the written blocks, which viewers rely on for
        // binary search and lane assignment.
        std::vector<std::pair<uint64_t, Instruction*>> order;
        order.reserve(instructions.size());
        for(auto& entry : instructions) {
            auto& ins = entry.second;
            close_all_stages(ins);
            order.emplace_back(ins.retired ? ins.retire_time : cycle_time(), &ins);
            if(!ins.retired)
                stats.unretired_at_eof++;
        }
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first : a.second->tx_id < b.second->tx_id;
        });
        for(auto& entry : order)
            writer.endTransaction(entry.second->tx_id, entry.first);
        instructions.clear();
    }

    Stats stats;

private:
    ftr::ftr_writer<true> writer;
    int64_t cur_cycle = 0;
    uint64_t next_event_tx = EVENT_TX_BASE;
    // Kanata file id -> in-flight instruction state
    std::unordered_map<uint64_t, Instruction> instructions;

    static std::vector<std::string> split_tabs(const std::string& line) {
        std::vector<std::string> fields;
        size_t start = 0;
        while(true) {
            size_t tab = line.find('\t', start);
            if(tab == std::string::npos) {
                fields.push_back(line.substr(start));
                return fields;
            }
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
    }

    static int64_t to_i64(const std::string& s) { return std::stoll(s); }
    static uint64_t to_u64(const std::string& s) {
        if(s.empty() || s[0] == '-')
            throw std::runtime_error("expected unsigned integer, got '" + s + "'");
        return std::stoull(s);
    }

    uint64_t cycle_time() const {
        if(cur_cycle < 0)
            throw std::runtime_error("event at negative cycle " + std::to_string(cur_cycle));
        return static_cast<uint64_t>(cur_cycle);
    }

    Instruction& instruction(const std::string& file_id_str) {
        auto it = instructions.find(to_u64(file_id_str));
        if(it == instructions.end())
            throw std::runtime_error("command for unknown/retired instruction id " + file_id_str);
        return it->second;
    }

    void cmd_begin_instruction(const std::vector<std::string>& fields) {
        uint64_t file_id = to_u64(fields.at(1));
        if(instructions.count(file_id))
            throw std::runtime_error("instruction id " + fields[1] + " already open");
        Instruction ins;
        ins.tx_id = file_id + 1;
        writer.startTransaction(ins.tx_id, GEN_INSTRUCTION, STREAM_ID, cycle_time());
        writer.writeAttribute(ins.tx_id, ftr::event_type::BEGIN, "insn_id_in_sim",
                              ftr::data_type::UNSIGNED, to_u64(fields.at(2)));
        writer.writeAttribute(ins.tx_id, ftr::event_type::BEGIN, "thread_id",
                              ftr::data_type::UNSIGNED, to_u64(fields.at(3)));
        instructions.emplace(file_id, ins);
        stats.instructions++;
    }

    void cmd_label(const std::vector<std::string>& fields, const std::string& line) {
        auto& ins = instruction(fields.at(1));
        uint64_t type = to_u64(fields.at(2));
        // The text is everything after the third tab; it may itself contain
        // tabs, which field splitting would lose.
        size_t text_pos = 0;
        for(int i = 0; i < 3; ++i)
            text_pos = line.find('\t', text_pos) + 1;
        std::string text = line.substr(text_pos);

        if(type == 0) {
            writer.writeAttribute(ins.tx_id, ftr::event_type::RECORD, "label",
                                  ftr::data_type::STRING, text);
        } else if(type == 1) {
            writer.writeAttribute(ins.tx_id, ftr::event_type::RECORD, "detail",
                                  ftr::data_type::STRING, text);
        } else if(type == 2) {
            // Konata attaches these to the last started stage
            if(ins.last_stage_open) {
                writer.writeAttribute(ins.last_stage_tx, ftr::event_type::RECORD, "label",
                                      ftr::data_type::STRING, text);
            } else {
                writer.writeAttribute(ins.tx_id, ftr::event_type::RECORD, "detail",
                                      ftr::data_type::STRING, text);
                stats.stage_labels_on_closed_stage++;
            }
        } else {
            throw std::runtime_error("unknown label type " + fields[2]);
        }
        stats.labels++;
    }

    void close_stage(Instruction& ins, uint64_t lane, uint64_t event_tx) {
        writer.endTransaction(event_tx, cycle_time());
        writer.writeRelation("parent_of", STREAM_ID, event_tx, STREAM_ID, ins.tx_id);
        ins.open_stage_by_lane.erase(lane);
        if(ins.last_stage_tx == event_tx)
            ins.last_stage_open = false;
    }

    void close_all_stages(Instruction& ins) {
        while(!ins.open_stage_by_lane.empty()) {
            auto it = ins.open_stage_by_lane.begin();
            close_stage(ins, it->first, it->second);
        }
    }

    void cmd_stage_begin(const std::vector<std::string>& fields) {
        auto& ins = instruction(fields.at(1));
        if(ins.retired)
            throw std::runtime_error("stage begin for retired instruction id " + fields.at(1));
        uint64_t lane = to_u64(fields.at(2));
        const std::string& stage = fields.at(3);

        // Konata implicitly ends a still-open stage on the same lane
        auto open = ins.open_stage_by_lane.find(lane);
        if(open != ins.open_stage_by_lane.end())
            close_stage(ins, lane, open->second);

        uint64_t event_tx = next_event_tx++;
        writer.startTransaction(event_tx, GEN_INSTRUCTION_EVENTS, STREAM_ID, cycle_time());
        writer.writeAttribute(event_tx, ftr::event_type::BEGIN, "name",
                              ftr::data_type::STRING, stage);
        writer.writeAttribute(event_tx, ftr::event_type::RECORD, "lane",
                              ftr::data_type::UNSIGNED, lane);
        ins.open_stage_by_lane[lane] = event_tx;
        ins.last_stage_tx = event_tx;
        ins.last_stage_open = true;
        stats.stages++;
    }

    void cmd_stage_end(const std::vector<std::string>& fields) {
        auto& ins = instruction(fields.at(1));
        uint64_t lane = to_u64(fields.at(2));
        auto open = ins.open_stage_by_lane.find(lane);
        if(open == ins.open_stage_by_lane.end()) {
            // stage already closed implicitly; matches Konata's tolerance
            stats.unmatched_stage_ends++;
            return;
        }
        close_stage(ins, lane, open->second);
    }

    void cmd_retire(const std::vector<std::string>& fields) {
        auto& ins = instruction(fields.at(1));
        if(ins.retired)
            throw std::runtime_error("duplicate retire for instruction id " + fields.at(1));
        uint64_t retire_id = to_u64(fields.at(2));
        bool flushed = to_u64(fields.at(3)) != 0;

        close_all_stages(ins);
        writer.writeAttribute(ins.tx_id, ftr::event_type::END, "retire_id",
                              ftr::data_type::UNSIGNED, retire_id);
        writer.writeAttribute(ins.tx_id, ftr::event_type::END, "flushed",
                              ftr::data_type::BOOLEAN, flushed);
        ins.retired = true;
        ins.retire_time = cycle_time();
        (flushed ? stats.flushes : stats.retires)++;
    }

    void cmd_wakeup(const std::vector<std::string>& fields) {
        uint64_t consumer_tx = to_u64(fields.at(1)) + 1;
        uint64_t producer_tx = to_u64(fields.at(2)) + 1;
        writer.writeRelation("wakeup", STREAM_ID, consumer_tx, STREAM_ID, producer_tx);
        stats.wakeups++;
    }
};

} // namespace

int main(int argc, char** argv) {
    if(argc < 2 || argc > 3) {
        std::cerr << "usage: " << argv[0] << " <output.ftr> [input.log]\n"
                  << "       reads the Kanata log from stdin when no input file is given\n";
        return 1;
    }

    std::ifstream file_input;
    if(argc == 3) {
        file_input.open(argv[2]);
        if(!file_input) {
            std::cerr << "error: cannot open input file " << argv[2] << "\n";
            return 1;
        }
    }
    std::istream& input = argc == 3 ? file_input : std::cin;

    try {
        Converter converter(argv[1]);
        std::string line;
        size_t line_no = 0;
        while(std::getline(input, line)) {
            line_no++;
            if(!line.empty() && line.back() == '\r')
                line.pop_back();
            converter.process_line(line, line_no);
        }
        converter.finish();

        const Stats& s = converter.stats;
        std::cerr << "instructions: " << s.instructions << " (retired " << s.retires
                  << ", flushed " << s.flushes << ", open at EOF " << s.unretired_at_eof << ")\n"
                  << "stage events: " << s.stages << " (unmatched ends " << s.unmatched_stage_ends
                  << ")\n"
                  << "labels: " << s.labels << " (stage labels after stage close "
                  << s.stage_labels_on_closed_stage << ")\n"
                  << "wakeup relations: " << s.wakeups << "\n";
    } catch(std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
