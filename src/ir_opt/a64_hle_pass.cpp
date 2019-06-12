/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * This software may be used and distributed according to the terms of the GNU
 * General Public License version 2 or any later version.
 */

#include <algorithm>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <optional>

#include <dynarmic/A64/config.h>
#include <dynarmic/A64/hle.h>

#include "common/common_types.h"
#include "frontend/A64/location_descriptor.h"
#include "frontend/A64/translate/translate.h"
#include "frontend/A64/types.h"
#include "frontend/A64/ir_emitter.h"
#include "frontend/ir/basic_block.h"
#include "frontend/ir/opcodes.h"
#include "ir_opt/passes.h"

namespace Dynarmic::Optimization {

static std::optional<u64> DoesDestinationMatchStub(IR::Block& caller, const A64::UserConfig& conf) {
    const auto caller_terminal = caller.GetTerminal();
    if (auto term = boost::get<IR::Term::LinkBlock>(&caller_terminal)) {
        const auto get_code = [&conf](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
        IR::Block callee = A64::Translate(A64::LocationDescriptor{term->next}, get_code, {conf.define_unpredictable_behaviour});
        Optimization::A64GetSetElimination(callee);
        Optimization::ConstantPropagation(callee);
        Optimization::DeadCodeElimination(callee);
        Optimization::VerificationPass(callee);

        const auto callee_terminal = callee.GetTerminal();
        if (!boost::get<IR::Term::FastDispatchHint>(&callee_terminal)) {
            return {};
        }

        if (callee.empty()) {
            return {};
        }

        const auto set_pc = &callee.back();
        if (set_pc->GetOpcode() != IR::Opcode::A64SetPC) {
            return {};
        }

        if (set_pc->GetArg(0).IsImmediate()) {
            return {};
        }

        const auto read_memory = set_pc->GetArg(0).GetInstIgnoreIdentity();
        if (read_memory->GetOpcode() != IR::Opcode::A64ReadMemory64) {
            return {};
        }

        if (!read_memory->GetArg(0).IsImmediate()) {
            return {};
        }

        const u64 read_location = read_memory->GetArg(0).GetU64();

        for (auto& inst : callee) {
            if (!inst.MayHaveSideEffects()) {
                continue;
            }
            if (&inst == set_pc || &inst == read_memory) {
                continue;
            }
            switch (inst.GetOpcode()) {
            case IR::Opcode::A64SetW:
            case IR::Opcode::A64SetX:
                // Intra-procedure-call temporary registers (AArch64 ABI)
                if (inst.GetArg(0).GetA64RegRef() == A64::Reg::R16 || inst.GetArg(0).GetA64RegRef() == A64::Reg::R17) {
                    continue;
                }
            default:
                break;
            }

            return {};
        }

        return read_location;
    }
    return {};
}

void A64HLEPass(IR::Block& block, const A64::UserConfig& conf, const A64::HLE::FunctionMap& hle_functions) {
    const auto read_location = DoesDestinationMatchStub(block, conf);
    if (!read_location) {
        return;
    }

    const auto function = hle_functions.find(*read_location);
    if (function == hle_functions.end()) {
        return;
    }

    //fmt::print("caller for {}:\n", *read_location);
    //fmt::print("{}\n", IR::DumpBlock(block));

    const auto push_rsb = std::find_if(block.begin(), block.end(), [](const IR::Inst& inst){ return inst.GetOpcode() == IR::Opcode::PushRSB; });

    if (push_rsb != block.end()) {
        block.Instructions().remove(push_rsb);
        block.ReplaceTerminal(IR::Term::CallHLEFunction{*read_location, IR::Term::LinkBlockFast{block.EndLocation()}});
    } else {
        A64::IREmitter ir{block};
        ir.SetPC(ir.GetX(A64::Reg::R30));
        block.ReplaceTerminal(IR::Term::CallHLEFunction{*read_location, IR::Term::PopRSBHint{}});
    }
}

} // namespace Dynarmic::Optimization
