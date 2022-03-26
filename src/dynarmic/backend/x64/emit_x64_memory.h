/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <xbyak/xbyak.h>

#include "dynarmic/backend/x64/a32_emit_x64.h"
#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/common/spin_lock_x64.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic::Backend::X64 {

namespace {

using namespace Xbyak::util;

constexpr size_t page_bits = 12;
constexpr size_t page_size = 1 << page_bits;
constexpr size_t page_mask = (1 << page_bits) - 1;

template<typename EmitContext>
void EmitDetectMisalignedVAddr(BlockOfCode& code, EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 tmp) {
    if (bitsize == 8 || (ctx.conf.detect_misaligned_access_via_page_table & bitsize) == 0) {
        return;
    }

    const u32 align_mask = [bitsize]() -> u32 {
        switch (bitsize) {
        case 16:
            return 0b1;
        case 32:
            return 0b11;
        case 64:
            return 0b111;
        case 128:
            return 0b1111;
        default:
            UNREACHABLE();
        }
    }();

    code.test(vaddr, align_mask);

    if (!ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
        code.jnz(abort, code.T_NEAR);
        return;
    }

    const u32 page_align_mask = static_cast<u32>(page_size - 1) & ~align_mask;

    Xbyak::Label detect_boundary, resume;

    code.jnz(detect_boundary, code.T_NEAR);
    code.L(resume);

    code.SwitchToFarCode();
    code.L(detect_boundary);
    code.mov(tmp, vaddr);
    code.and_(tmp, page_align_mask);
    code.cmp(tmp, page_align_mask);
    code.jne(resume, code.T_NEAR);
    // NOTE: We expect to fallthrough into abort code here.
    code.SwitchToNearCode();
}

template<typename EmitContext>
Xbyak::RegExp EmitVAddrLookup(BlockOfCode& code, EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr);

template<>
[[maybe_unused]] Xbyak::RegExp EmitVAddrLookup<A32EmitContext>(BlockOfCode& code, A32EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr) {
    const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr();
    const Xbyak::Reg32 tmp = ctx.conf.absolute_offset_page_table ? page.cvt32() : ctx.reg_alloc.ScratchGpr().cvt32();

    EmitDetectMisalignedVAddr(code, ctx, bitsize, abort, vaddr, tmp.cvt64());

    // TODO: This code assumes vaddr has been zext from 32-bits to 64-bits.

    code.mov(tmp, vaddr.cvt32());
    code.shr(tmp, static_cast<int>(page_bits));

    code.mov(page, qword[r14 + tmp.cvt64() * sizeof(void*)]);
    if (ctx.conf.page_table_pointer_mask_bits == 0) {
        code.test(page, page);
    } else {
        code.and_(page, ~u32(0) << ctx.conf.page_table_pointer_mask_bits);
    }
    code.jz(abort, code.T_NEAR);
    if (ctx.conf.absolute_offset_page_table) {
        return page + vaddr;
    }
    code.mov(tmp, vaddr.cvt32());
    code.and_(tmp, static_cast<u32>(page_mask));
    return page + tmp.cvt64();
}

template<>
[[maybe_unused]] Xbyak::RegExp EmitVAddrLookup<A64EmitContext>(BlockOfCode& code, A64EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr) {
    const size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_bits;
    const size_t unused_top_bits = 64 - ctx.conf.page_table_address_space_bits;

    const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr();
    const Xbyak::Reg64 tmp = ctx.conf.absolute_offset_page_table ? page : ctx.reg_alloc.ScratchGpr();

    EmitDetectMisalignedVAddr(code, ctx, bitsize, abort, vaddr, tmp);

    if (unused_top_bits == 0) {
        code.mov(tmp, vaddr);
        code.shr(tmp, int(page_bits));
    } else if (ctx.conf.silently_mirror_page_table) {
        if (valid_page_index_bits >= 32) {
            if (code.HasHostFeature(HostFeature::BMI2)) {
                const Xbyak::Reg64 bit_count = ctx.reg_alloc.ScratchGpr();
                code.mov(bit_count, unused_top_bits);
                code.bzhi(tmp, vaddr, bit_count);
                code.shr(tmp, int(page_bits));
                ctx.reg_alloc.Release(bit_count);
            } else {
                code.mov(tmp, vaddr);
                code.shl(tmp, int(unused_top_bits));
                code.shr(tmp, int(unused_top_bits + page_bits));
            }
        } else {
            code.mov(tmp, vaddr);
            code.shr(tmp, int(page_bits));
            code.and_(tmp, u32((1 << valid_page_index_bits) - 1));
        }
    } else {
        ASSERT(valid_page_index_bits < 32);
        code.mov(tmp, vaddr);
        code.shr(tmp, int(page_bits));
        code.test(tmp, u32(-(1 << valid_page_index_bits)));
        code.jnz(abort, code.T_NEAR);
    }
    code.mov(page, qword[r14 + tmp * sizeof(void*)]);
    if (ctx.conf.page_table_pointer_mask_bits == 0) {
        code.test(page, page);
    } else {
        code.and_(page, ~u32(0) << ctx.conf.page_table_pointer_mask_bits);
    }
    code.jz(abort, code.T_NEAR);
    if (ctx.conf.absolute_offset_page_table) {
        return page + vaddr;
    }
    code.mov(tmp, vaddr);
    code.and_(tmp, static_cast<u32>(page_mask));
    return page + tmp;
}

template<typename UserConfig>
void EmitExclusiveLock(BlockOfCode& code, const UserConfig& conf, Xbyak::Reg64 pointer, Xbyak::Reg32 tmp) {
    if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return;
    }

    code.mov(pointer, Common::BitCast<u64>(GetExclusiveMonitorLockPointer(conf.global_monitor)));
    EmitSpinLockLock(code, pointer, tmp);
}

template<typename UserConfig>
void EmitExclusiveUnlock(BlockOfCode& code, const UserConfig& conf, Xbyak::Reg64 pointer, Xbyak::Reg32 tmp) {
    if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return;
    }

    code.mov(pointer, Common::BitCast<u64>(GetExclusiveMonitorLockPointer(conf.global_monitor)));
    EmitSpinLockUnlock(code, pointer, tmp);
}

template<typename UserConfig>
void EmitExclusiveTestAndClear(BlockOfCode& code, const UserConfig& conf, Xbyak::Reg64 vaddr, Xbyak::Reg64 pointer, Xbyak::Reg64 tmp) {
    if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return;
    }

    code.mov(tmp, 0xDEAD'DEAD'DEAD'DEAD);
    const size_t processor_count = GetExclusiveMonitorProcessorCount(conf.global_monitor);
    for (size_t processor_index = 0; processor_index < processor_count; processor_index++) {
        if (processor_index == conf.processor_id) {
            continue;
        }
        Xbyak::Label ok;
        code.mov(pointer, Common::BitCast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, processor_index)));
        code.cmp(qword[pointer], vaddr);
        code.jne(ok);
        code.mov(qword[pointer], tmp);
        code.L(ok);
    }
}

}  // namespace

}  // namespace Dynarmic::Backend::X64
