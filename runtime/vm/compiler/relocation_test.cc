// Copyright (c) 2021, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "platform/assert.h"

#include "vm/allocation.h"
#include "vm/code_patcher.h"
#include "vm/compiler/assembler/assembler.h"
#include "vm/compiler/relocation.h"
#include "vm/instructions.h"
#include "vm/longjump.h"
#include "vm/unit_test.h"

#define __ assembler->

namespace dart {

#if defined(DART_PRECOMPILER) && !defined(TARGET_ARCH_IA32)

DECLARE_FLAG(bool, dual_map_code);
DECLARE_FLAG(int, lower_pc_relative_call_distance);
DECLARE_FLAG(int, upper_pc_relative_call_distance);

struct RelocatorTestHelper {
  explicit RelocatorTestHelper(Thread* thread)
      : thread(thread),
        locker(thread, thread->isolate_group()->program_lock()),
        safepoint_and_growth_scope(thread) {
    // So the relocator uses the correct instruction size layout.
    FLAG_precompiled_mode = true;
    FLAG_use_bare_instructions = true;

    FLAG_lower_pc_relative_call_distance = -128;
    FLAG_upper_pc_relative_call_distance = 128;
  }
  ~RelocatorTestHelper() {
    FLAG_use_bare_instructions = false;
    FLAG_precompiled_mode = false;
  }

  void CreateInstructions(std::initializer_list<intptr_t> sizes) {
    for (auto size : sizes) {
      codes.Add(&Code::Handle(AllocationInstruction(size)));
    }
  }

  CodePtr AllocationInstruction(uintptr_t size) {
    const auto& instructions = Instructions::Handle(
        Instructions::New(size, /*has_monomorphic=*/false));

    uword addr = instructions.PayloadStart();
    for (uintptr_t i = 0; i < (size / 4); ++i) {
      *reinterpret_cast<uint32_t*>(addr + 4 * i) =
          static_cast<uint32_t>(kBreakInstructionFiller);
    }

    const auto& code = Code::Handle(Code::New(0));
    code.SetActiveInstructions(instructions, 0);
    code.set_instructions(instructions);
    return code.ptr();
  }

  void EmitPcRelativeCallFunction(intptr_t idx, intptr_t to_idx) {
    const Code& code = *codes[idx];
    const Code& target = *codes[to_idx];

    EmitCodeFor(code, [&](compiler::Assembler* assembler) {
#if defined(TARGET_ARCH_ARM64)
      // TODO(kustermann): Remove conservative approximation in relocator and
      // make tests precise.
      __ mov(R0, R0);
      __ mov(R0, R0);
      __ mov(R0, R0);
      SPILLS_RETURN_ADDRESS_FROM_LR_TO_REGISTER(
          __ stp(LR, R1,
                 compiler::Address(CSP, -2 * kWordSize,
                                   compiler::Address::PairPreIndex)));
#elif defined(TARGET_ARCH_ARM)
      SPILLS_RETURN_ADDRESS_FROM_LR_TO_REGISTER(__ PushList((1 << LR)));
#endif
      __ GenerateUnRelocatedPcRelativeCall();
      AddPcRelativeCallTargetAt(__ CodeSize(), code, target);
#if defined(TARGET_ARCH_ARM64)
      RESTORES_RETURN_ADDRESS_FROM_REGISTER_TO_LR(
          __ ldp(LR, R1,
                 compiler::Address(CSP, 2 * kWordSize,
                                   compiler::Address::PairPostIndex)));
#elif defined(TARGET_ARCH_ARM)
      RESTORES_RETURN_ADDRESS_FROM_REGISTER_TO_LR(__ PopList((1 << LR)));
#endif
      __ Ret();
    });
  }

  void EmitReturn42Function(intptr_t idx) {
    const Code& code = *codes[idx];
    EmitCodeFor(code, [&](compiler::Assembler* assembler) {
#if defined(TARGET_ARCH_X64)
      __ LoadImmediate(RAX, 42);
#elif defined(TARGET_ARCH_ARM) || defined(TARGET_ARCH_ARM64)
      __ LoadImmediate(R0, 42);
#endif
      __ Ret();
    });
  }

  void EmitCodeFor(const Code& code,
                   std::function<void(compiler::Assembler* assembler)> fun) {
    const auto& inst = Instructions::Handle(code.instructions());

    compiler::Assembler assembler(nullptr);
    fun(&assembler);

    const uword addr = inst.PayloadStart();
    memmove(reinterpret_cast<void*>(addr),
            reinterpret_cast<void*>(assembler.CodeAddress(0)),
            assembler.CodeSize());

    if (FLAG_write_protect_code && FLAG_dual_map_code) {
      auto& instructions = Instructions::Handle(code.instructions());
      instructions ^= OldPage::ToExecutable(instructions.ptr());
      code.set_instructions(instructions);
    }
  }

  void AddPcRelativeCallTargetAt(intptr_t offset,
                                 const Code& code,
                                 const Code& target) {
    const auto& kind_and_offset = Smi::Handle(
        Smi::New(Code::KindField::encode(Code::kPcRelativeCall) |
                 Code::EntryPointField::encode(Code::kDefaultEntry) |
                 Code::OffsetField::encode(offset)));
    AddCall(code, target, kind_and_offset);
  }

  void AddCall(const Code& code,
               const Code& target,
               const Smi& kind_and_offset) {
    auto& call_targets = Array::Handle(code.static_calls_target_table());
    if (call_targets.IsNull()) {
      call_targets = Array::New(Code::kSCallTableEntryLength);
    } else {
      call_targets = Array::Grow(
          call_targets, call_targets.Length() + Code::kSCallTableEntryLength);
    }

    StaticCallsTable table(call_targets);
    auto entry = table[table.Length() - 1];
    entry.Set<Code::kSCallTableKindAndOffset>(kind_and_offset);
    entry.Set<Code::kSCallTableCodeOrTypeTarget>(target);
    entry.Set<Code::kSCallTableFunctionTarget>(
        Function::Handle(Function::null()));
    code.set_static_calls_target_table(call_targets);
  }

  void BuildImageAndRunTest(
      std::function<void(const GrowableArray<ImageWriterCommand>&, uword*)>
          fun) {
    auto& image = Instructions::Handle();
    uword entrypoint = 0;
    {
      GrowableArray<CodePtr> raw_codes;
      for (auto code : codes) {
        raw_codes.Add(code->ptr());
      }

      GrowableArray<ImageWriterCommand> commands;
      CodeRelocator::Relocate(thread, &raw_codes, &commands,
                              /*is_vm_isolate=*/false);

      uword expected_offset = 0;
      fun(commands, &expected_offset);

      image = BuildImage(&commands);
      entrypoint = image.EntryPoint() + expected_offset;

      for (intptr_t i = 0; i < commands.length(); ++i) {
        if (commands[i].op == ImageWriterCommand::InsertBytesOfTrampoline) {
          delete[] commands[i].insert_trampoline_bytes.buffer;
          commands[i].insert_trampoline_bytes.buffer = nullptr;
        }
      }
    }
    typedef intptr_t (*Fun)() DART_UNUSED;
#if defined(TARGET_ARCH_X64)
    EXPECT_EQ(42, reinterpret_cast<Fun>(entrypoint)());
#elif defined(TARGET_ARCH_ARM)
    EXPECT_EQ(42, EXECUTE_TEST_CODE_INT32(Fun, entrypoint));
#elif defined(TARGET_ARCH_ARM64)
    EXPECT_EQ(42, EXECUTE_TEST_CODE_INT64(Fun, entrypoint));
#endif
  }

  InstructionsPtr BuildImage(GrowableArray<ImageWriterCommand>* commands) {
    intptr_t size = 0;
    for (intptr_t i = 0; i < commands->length(); ++i) {
      switch ((*commands)[i].op) {
        case ImageWriterCommand::InsertBytesOfTrampoline:
          size += (*commands)[i].insert_trampoline_bytes.buffer_length;
          break;
        case ImageWriterCommand::InsertInstructionOfCode:
          size += ImageWriter::SizeInSnapshot(Code::InstructionsOf(
              (*commands)[i].insert_instruction_of_code.code));
          break;
      }
    }

    auto& instructions = Instructions::Handle(
        Instructions::New(size, /*has_monomorphic=*/false));
    {
      uword addr = instructions.PayloadStart();
      for (intptr_t i = 0; i < commands->length(); ++i) {
        switch ((*commands)[i].op) {
          case ImageWriterCommand::InsertBytesOfTrampoline: {
            const auto entry = (*commands)[i].insert_trampoline_bytes;
            const auto current_size = entry.buffer_length;
            memmove(reinterpret_cast<void*>(addr), entry.buffer, current_size);
            addr += current_size;
            break;
          }
          case ImageWriterCommand::InsertInstructionOfCode: {
            const auto entry = (*commands)[i].insert_instruction_of_code;
            const auto current_size =
                ImageWriter::SizeInSnapshot(Code::InstructionsOf(entry.code));
            const auto alias_offset =
                OldPage::Of(Code::InstructionsOf(entry.code))->AliasOffset();
            memmove(
                reinterpret_cast<void*>(addr),
                reinterpret_cast<void*>(Instructions::PayloadStart(
                                            Code::InstructionsOf(entry.code)) -
                                        alias_offset),
                current_size);
            addr += current_size;
            break;
          }
        }
      }

      if (FLAG_write_protect_code) {
        const uword address = UntaggedObject::ToAddr(instructions.ptr());
        const auto size = instructions.ptr()->untag()->HeapSize();
        instructions =
            Instructions::RawCast(OldPage::ToExecutable(instructions.ptr()));

        const auto prot = FLAG_dual_map_code ? VirtualMemory::kReadOnly
                                             : VirtualMemory::kReadExecute;
        VirtualMemory::Protect(reinterpret_cast<void*>(address), size, prot);
      }
      CPU::FlushICache(instructions.PayloadStart(), instructions.Size());
    }
    return instructions.ptr();
  }

  Thread* thread;
  SafepointWriteRwLocker locker;
  ForceGrowthSafepointOperationScope safepoint_and_growth_scope;
  GrowableArray<const Code*> codes;
};

ISOLATE_UNIT_TEST_CASE(CodeRelocator_DirectForwardCall) {
  RelocatorTestHelper helper(thread);
  helper.CreateInstructions({32, 36, 32});
  helper.EmitPcRelativeCallFunction(0, 2);
  helper.EmitReturn42Function(2);
  helper.BuildImageAndRunTest(
      [&](const GrowableArray<ImageWriterCommand>& commands,
          uword* entry_point) {
        EXPECT_EQ(3, commands.length());

        // This makes an in-range forward call.
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[0].op);
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[1].op);
        // This is is the target of the forwards call.
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[2].op);

        *entry_point = commands[0].expected_offset;
      });
}

ISOLATE_UNIT_TEST_CASE(CodeRelocator_OutOfRangeForwardCall) {
  RelocatorTestHelper helper(thread);
  helper.CreateInstructions(
      {32, FLAG_upper_pc_relative_call_distance - 32 + 4, 32});
  helper.EmitPcRelativeCallFunction(0, 2);
  helper.EmitReturn42Function(2);
  helper.BuildImageAndRunTest([&](const GrowableArray<ImageWriterCommand>&
                                      commands,
                                  uword* entry_point) {
    EXPECT_EQ(4, commands.length());

    // This makes an out-of-range forward call.
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[0].op);
    // This is the last change the relocator thinks it can ensure the
    // out-of-range call above can call a trampoline - so it injets it here and
    // no later.
    EXPECT_EQ(ImageWriterCommand::InsertBytesOfTrampoline, commands[1].op);
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[2].op);
    // This is the target of the forwwards call.
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[3].op);

    *entry_point = commands[0].expected_offset;
  });
}

ISOLATE_UNIT_TEST_CASE(CodeRelocator_DirectBackwardCall) {
  RelocatorTestHelper helper(thread);
  helper.CreateInstructions({32, 32, 32});
  helper.EmitReturn42Function(0);
  helper.EmitPcRelativeCallFunction(2, 0);
  helper.BuildImageAndRunTest(
      [&](const GrowableArray<ImageWriterCommand>& commands,
          uword* entry_point) {
        EXPECT_EQ(3, commands.length());

        // This is the backwards call target.
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[0].op);
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[1].op);
        // This makes an in-range backwards call.
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[2].op);

        *entry_point = commands[2].expected_offset;
      });
}

ISOLATE_UNIT_TEST_CASE(CodeRelocator_OutOfRangeBackwardCall) {
  RelocatorTestHelper helper(thread);
  helper.CreateInstructions({32, 32, 32, 32 + 4, 32, 32, 32, 32, 32});
  helper.EmitReturn42Function(0);
  helper.EmitPcRelativeCallFunction(4, 0);
  helper.BuildImageAndRunTest([&](const GrowableArray<ImageWriterCommand>&
                                      commands,
                                  uword* entry_point) {
    EXPECT_EQ(10, commands.length());

    // This is the backwards call target.
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[0].op);
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[1].op);
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[2].op);
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[3].op);
    // This makes an out-of-range backwards call. The relocator will make the
    // call go to a trampoline instead. It will delay insertion of the
    // trampoline until it almost becomes out-of-range.
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[4].op);
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[5].op);
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[6].op);
    // This is the last change the relocator thinks it can ensure the
    // out-of-range call above can call a trampoline - so it injets it here and
    // no later.
    EXPECT_EQ(ImageWriterCommand::InsertBytesOfTrampoline, commands[7].op);
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[8].op);
    EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[9].op);

    *entry_point = commands[4].expected_offset;
  });
}

ISOLATE_UNIT_TEST_CASE(CodeRelocator_OutOfRangeBackwardCall2) {
  RelocatorTestHelper helper(thread);
  helper.CreateInstructions({32, 32, 32, 32 + 4, 32});
  helper.EmitReturn42Function(0);
  helper.EmitPcRelativeCallFunction(4, 0);
  helper.BuildImageAndRunTest(
      [&](const GrowableArray<ImageWriterCommand>& commands,
          uword* entry_point) {
        EXPECT_EQ(6, commands.length());

        // This is the backwards call target.
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[0].op);
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[1].op);
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[2].op);
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[3].op);
        // This makes an out-of-range backwards call. The relocator will make
        // the call go to a trampoline instead. It will delay insertion of the
        // trampoline until it almost becomes out-of-range.
        EXPECT_EQ(ImageWriterCommand::InsertInstructionOfCode, commands[4].op);
        // There's no other instructions coming, so the relocator will resolve
        // any pending out-of-range calls by inserting trampolines at the end.
        EXPECT_EQ(ImageWriterCommand::InsertBytesOfTrampoline, commands[5].op);

        *entry_point = commands[4].expected_offset;
      });
}

UNIT_TEST_CASE(PCRelativeCallPatterns) {
  {
    uint8_t instruction[PcRelativeCallPattern::kLengthInBytes];

    PcRelativeCallPattern pattern(reinterpret_cast<uword>(&instruction));

    pattern.set_distance(PcRelativeCallPattern::kLowerCallingRange);
    EXPECT_EQ(PcRelativeCallPattern::kLowerCallingRange, pattern.distance());

    pattern.set_distance(PcRelativeCallPattern::kUpperCallingRange);
    EXPECT_EQ(PcRelativeCallPattern::kUpperCallingRange, pattern.distance());
  }
  {
    uint8_t instruction[PcRelativeTailCallPattern::kLengthInBytes];

    PcRelativeTailCallPattern pattern(reinterpret_cast<uword>(&instruction));

    pattern.set_distance(PcRelativeTailCallPattern::kLowerCallingRange);
    EXPECT_EQ(PcRelativeTailCallPattern::kLowerCallingRange,
              pattern.distance());

    pattern.set_distance(PcRelativeTailCallPattern::kUpperCallingRange);
    EXPECT_EQ(PcRelativeTailCallPattern::kUpperCallingRange,
              pattern.distance());
  }
}

#endif  // defined(DART_PRECOMPILER) && !defined(TARGET_ARCH_IA32)

}  // namespace dart
