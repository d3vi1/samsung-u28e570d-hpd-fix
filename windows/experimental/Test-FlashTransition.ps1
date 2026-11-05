param()
$ErrorActionPreference = 'Stop'

# Compiles and exercises only the pure engine plus an in-memory fake device.
# No NVIDIA code, imports, display discovery, or monitor operations are loaded.
$engine = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'FlashTransition.cs') -Raw
$tests = @'
namespace SamsungExperimentalTests
{
using System;
using System.Collections.Generic;
using SamsungExperimental;

public static class FlashTransitionOfflineTests
{
    private sealed class FakeFlash : IFlashDevice
    {
        public byte[] Bytes;
        public byte[] StatusBytes = new byte[] { 0, 2, 0x60 };
        public List<int> Writes = new List<int>();
        public List<int> PageSizes = new List<int>();
        public int ReadCalls;
        public int FailReadCall;
        public int FullReads;
        public int StatusReads;
        public int DisableCalls;
        public int FailWriteCall;
        public int FailRollbackCall;
        public bool CorruptFirstTargetFullRead;
        public bool CorruptUntouchedTailOnTargetVerify;
        public bool CorruptFirstSectorRead;
        public bool ChangeStatusAfterWrite;
        public bool RejectShortRead;
        public int FailStatusCall;
        public Action BeforeFirstWrite;
        public Action<int> ObserveWrite;
        private bool hasFailed;

        public FakeFlash(byte[] source) { Bytes = (byte[])source.Clone(); }
        public byte[] ReadRange(int address, int length)
        {
            ReadCalls++;
            if (ReadCalls == FailReadCall) throw new Exception("Injected flash read failure");
            if (length == FlashTransition.Capacity) FullReads++;
            if (CorruptUntouchedTailOnTargetVerify && FullReads == 2 && length == FlashTransition.Capacity)
            {
                Bytes[Bytes.Length - 1] ^= 0x10;
                CorruptUntouchedTailOnTargetVerify = false;
            }
            byte[] result = new byte[length];
            Array.Copy(Bytes, address, result, 0, length);
            if (RejectShortRead) return new byte[length - 1];
            if (CorruptFirstTargetFullRead && FullReads == 2)
            {
                result[result.Length - 1] ^= 0x40;
                CorruptFirstTargetFullRead = false;
            }
            if (CorruptFirstSectorRead && length == FlashTransition.SectorSize && Writes.Count > 0)
            {
                result[0] ^= 0x40;
                CorruptFirstSectorRead = false;
            }
            return result;
        }
        public byte[] ReadStatus()
        {
            StatusReads++;
            if (StatusReads == FailStatusCall) throw new Exception("Injected status read failure");
            return (byte[])StatusBytes.Clone();
        }
        public void WriteDisable() { DisableCalls++; }
        public void WriteSector(int address, byte[] fullImage, int pageSize, Action<string> log)
        {
            if (Writes.Count == 0 && BeforeFirstWrite != null) BeforeFirstWrite();
            Writes.Add(address);
            PageSizes.Add(pageSize);
            if (ObserveWrite != null) ObserveWrite(address);
            log("fake sector write");
            if (address % 4096 != 0 || address < 0 || address + 4096 > Bytes.Length)
                throw new Exception("Invalid fake sector address");
            if (Writes.Count == FailWriteCall || Writes.Count == FailRollbackCall)
            {
                // Simulate an erase followed by transport failure, not a harmless throw.
                for (int i = 0; i < 4096; i++) Bytes[address + i] = 0xff;
                Bytes[address + 7] = 0x69;
                hasFailed = true;
                throw new Exception("Injected failure after erase");
            }
            Array.Copy(fullImage, address, Bytes, address, 4096);
            if (ChangeStatusAfterWrite && !hasFailed) StatusBytes[1] ^= 4;
            // A device adapter cannot mutate the engine's authoritative snapshots.
            fullImage[0] ^= 0x20;
        }
    }

    private static int passed;
    private static void Check(bool value, string message)
    {
        if (!value) throw new Exception(message);
    }
    private static byte[] Source()
    {
        byte[] result = new byte[FlashTransition.Capacity];
        for (int i = 0; i < result.Length; i++) result[i] = 0xff;
        for (int i = 0; i < 0x8d4bc; i++) result[i] = (byte)((i * 37 + (i >> 8)) & 255);
        return result;
    }
    private static byte[] Target(byte[] source, bool header)
    {
        byte[] result = (byte[])source.Clone();
        if (header) result[0x10014] ^= 0x63;
        result[0x24017] ^= 0x35;
        result[0x25083] ^= 0x71;
        // Candidate is longer than the official package: rollback must erase this tail.
        result[0x8e010] = 0x12;
        return result;
    }
    private static AddressRange[] Ranges()
    {
        return new AddressRange[] { new AddressRange(0x10000, 0x10080), new AddressRange(0x20080, 0x90000) };
    }
    private static FlashPlan Make(byte[] source, byte[] target)
    {
        return FlashTransition.Plan(source, target, FlashTransition.Sha256(source), FlashTransition.Sha256(target), Ranges());
    }
    private static FlashTransitionException Fails(FakeFlash device, FlashPlan plan,
        string pin, ExecutionState state, Action<string> log)
    {
        try { FlashTransition.Execute(device, plan, pin, 256, state, log); }
        catch (FlashTransitionException error) { return error; }
        throw new Exception("Expected transition failure");
    }
    private static void EqualImage(byte[] actual, byte[] expected, string message)
    {
        Check(FlashTransition.Sha256(actual) == FlashTransition.Sha256(expected), message);
    }
    private static void Test(string name, Action body)
    {
        body();
        passed++;
        Console.WriteLine("PASS " + name);
    }
    public static void Run()
    {
        Test("forward and explicit reverse restore candidate-only tail", delegate {
            byte[] source = Source(); byte[] target = Target(source, true);
            FlashPlan forward = Make(source, target); FakeFlash fake = new FakeFlash(source);
            ExecutionState state = new ExecutionState();
            fake.ObserveWrite = delegate(int address) { Check(!state.ResetAllowed && state.MutationStarted, "Reset permitted during mutation"); };
            FlashTransition.Execute(fake, forward, forward.PlanSha256, 256, state, null);
            Check(state.Completed && state.TargetVerified && state.ResetAllowed && !state.RollbackAttempted, "Success state");
            EqualImage(fake.Bytes, target, "Forward image");
            Check(fake.Writes[fake.Writes.Count - 1] == 0x10000, "Forward header was not last");
            FlashPlan reverse = Make(target, source); ExecutionState restored = new ExecutionState();
            fake.ObserveWrite = null;
            FlashTransition.Execute(fake, reverse, reverse.PlanSha256, 32, restored, null);
            EqualImage(fake.Bytes, source, "Reverse left candidate tail bytes");
            Check(fake.Bytes[0x8e010] == 0xff && restored.ResetAllowed, "Tail or reverse state");
            Check(fake.Writes[fake.Writes.Count - 1] == 0x10000, "Reverse header was not last");
            Check(forward.PlanSha256 != reverse.PlanSha256, "Direction not included in plan pin");
        });
        Test("no artificial header write", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, false));
            FakeFlash fake = new FakeFlash(source);
            FlashTransition.Execute(fake, plan, plan.PlanSha256, 256, new ExecutionState(), null);
            Check(!fake.Writes.Contains(0x10000), "Unchanged header was written");
        });
        Test("whole-image preflight mismatch gives zero writes", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.Bytes[0x1fffff] ^= 1;
            ExecutionState state = new ExecutionState(); FlashTransitionException error = Fails(fake, plan, plan.PlanSha256, state, null);
            Check(fake.Writes.Count == 0 && fake.DisableCalls == 0 && !state.MutationStarted && !state.ResetAllowed, "Unknown preflight image allowed reset or mutation");
            Check(error.RollbackStatus == RollbackStatus.NotRequired, "Preflight rollback status");
        });
        Test("short preflight read rejected", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.RejectShortRead = true;
            ExecutionState state = new ExecutionState();
            Fails(fake, plan, plan.PlanSha256, state, null);
            Check(fake.Writes.Count == 0 && !state.ResetAllowed, "Short source read allowed writes or reset");
        });
        Test("preflight read failure prohibits reset and performs no writes", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.FailReadCall = 1;
            ExecutionState state = new ExecutionState();
            Check(!state.ResetAllowed, "Initial reset permission must be false");
            Fails(fake, plan, plan.PlanSha256, state, null);
            Check(fake.Writes.Count == 0 && fake.DisableCalls == 0 && !state.ResetAllowed && !state.SourceVerified,
                "Read failure allowed mutation or reset");
        });
        Test("public pure validation rejects tampering and returns isolated snapshots", delegate {
            byte[] source = Source(); byte[] target = Target(source, true); FlashPlan plan = Make(source, target);
            string pin = plan.PlanSha256;
            FlashPlan snapshot = FlashTransition.ValidateAndSnapshot(plan, pin);
            plan.TargetFull[0x24017] ^= 1;
            plan.ChangedSectors[0].Address += 4096;
            EqualImage(snapshot.TargetFull, target, "Validated snapshot shares caller image storage");
            Check(snapshot.ChangedSectors[0].Address == 0x24000, "Validated snapshot shares caller sector storage");
            bool rejected = false;
            try { FlashTransition.ValidateAndSnapshot(plan, pin); } catch (ArgumentException) { rejected = true; }
            Check(rejected, "Public validation accepted a changed DTO");
        });
        Test("altered sector list rejected before device access", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            string pin = plan.PlanSha256; plan.ChangedSectors[0].Address += 4096;
            FakeFlash fake = new FakeFlash(source); Fails(fake, plan, pin, new ExecutionState(), null);
            Check(fake.ReadCalls == 0 && fake.StatusReads == 0 && fake.Writes.Count == 0, "Altered plan reached device");
        });
        Test("altered image and wrong plan pin rejected", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            string pin = plan.PlanSha256; plan.TargetFull[0x24017] ^= 1;
            FakeFlash fake = new FakeFlash(source); Fails(fake, plan, pin, new ExecutionState(), null);
            Check(fake.ReadCalls == 0, "Altered image reached device");
            plan = Make(source, Target(source, true));
            Fails(fake, plan, new string('0', 64), new ExecutionState(), null);
            Check(fake.ReadCalls == 0, "Wrong pin reached device");
        });
        Test("unapproved changed byte rejected", delegate {
            byte[] source = Source(); byte[] target = Target(source, true); target[0x10080] ^= 1;
            bool rejected = false;
            try { Make(source, target); } catch (ArgumentException) { rejected = true; }
            Check(rejected, "PM51 byte outside allowed header range was accepted");
        });
        Test("failure after erase restores every touched sector with 32-byte pages", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.FailWriteCall = 2;
            ExecutionState state = new ExecutionState(); FlashTransitionException error = Fails(fake, plan, plan.PlanSha256, state, null);
            EqualImage(fake.Bytes, source, "Rollback did not restore source");
            Check(state.TouchedSectors.Length == 2 && fake.Writes.Count == 4, "Throwing sector omitted from rollback");
            Check(fake.PageSizes[2] == 32 && fake.PageSizes[3] == 32, "Rollback page size");
            Check(state.ResetAllowed && state.RollbackVerified && error.RollbackStatus == RollbackStatus.Succeeded, "Rollback state");
        });
        Test("sector read-back failure triggers rollback", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.CorruptFirstSectorRead = true;
            ExecutionState state = new ExecutionState(); Fails(fake, plan, plan.PlanSha256, state, null);
            EqualImage(fake.Bytes, source, "Sector mismatch rollback");
            Check(state.RollbackVerified && state.ResetAllowed, "Sector mismatch state");
        });
        Test("full verification failure restores header last", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.CorruptFirstTargetFullRead = true;
            ExecutionState state = new ExecutionState(); Fails(fake, plan, plan.PlanSha256, state, null);
            EqualImage(fake.Bytes, source, "Full mismatch rollback");
            Check(fake.Writes.Count == 8 && fake.Writes[3] == 0x10000 && fake.Writes[7] == 0x10000, "Header ordering on rollback");
            Check(state.RollbackVerified && state.ResetAllowed, "Full mismatch state");
        });
        Test("throwing logger cannot block rollback", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.FailWriteCall = 2;
            ExecutionState state = new ExecutionState();
            Fails(fake, plan, plan.PlanSha256, state, delegate(string message) { throw new Exception("Journal unavailable"); });
            EqualImage(fake.Bytes, source, "Logger blocked restoration");
            Check(state.RollbackVerified && state.ResetAllowed, "Logger failure state");
        });
        Test("untouched tail corruption prevents reset after touched sectors restore", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.CorruptUntouchedTailOnTargetVerify = true;
            ExecutionState state = new ExecutionState(); Fails(fake, plan, plan.PlanSha256, state, null);
            Check(fake.Writes.Count == 8 && !state.ResetAllowed && !state.RollbackVerified, "Untouched tail corruption ignored");
        });
        Test("rollback failure keeps reset prohibited and attempts remaining sectors", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.FailWriteCall = 2; fake.FailRollbackCall = 3;
            ExecutionState state = new ExecutionState(); FlashTransitionException error = Fails(fake, plan, plan.PlanSha256, state, null);
            Check(fake.Writes.Count == 4, "Later touched sector was not attempted");
            Check(!state.ResetAllowed && !state.RollbackVerified && error.RollbackStatus == RollbackStatus.Failed, "Unverified rollback allowed reset");
            Check(error.RollbackError != null && Object.ReferenceEquals(error.State, state), "Failure evidence unavailable");
            Fails(fake, plan, plan.PlanSha256, state, null);
            Check(!state.ResetAllowed && fake.Writes.Count == 4, "Reusing state cleared recovery prohibition");
        });
        Test("status mismatch blocks reset even when source bytes restore", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.FailWriteCall = 2; fake.ChangeStatusAfterWrite = true;
            ExecutionState state = new ExecutionState(); Fails(fake, plan, plan.PlanSha256, state, null);
            EqualImage(fake.Bytes, source, "Status test rollback bytes");
            Check(!state.ResetAllowed && !state.RollbackVerified, "Status mismatch allowed reset");
        });
        Test("rollback status read failure prohibits reset", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, Target(source, true));
            FakeFlash fake = new FakeFlash(source); fake.FailWriteCall = 2; fake.FailStatusCall = 3;
            ExecutionState state = new ExecutionState(); Fails(fake, plan, plan.PlanSha256, state, null);
            EqualImage(fake.Bytes, source, "Rollback status-read failure bytes");
            Check(!state.ResetAllowed && !state.RollbackVerified, "Missing rollback status allowed reset");
        });
        Test("plan snapshot survives caller mutation during execution", delegate {
            byte[] source = Source(); byte[] target = Target(source, true); FlashPlan plan = Make(source, target);
            FakeFlash fake = new FakeFlash(source);
            fake.BeforeFirstWrite = delegate { plan.SourceFull[0] ^= 1; plan.TargetFull[0x25083] ^= 1; plan.ChangedSectors[1].Address = 0; };
            ExecutionState state = new ExecutionState();
            FlashTransition.Execute(fake, plan, plan.PlanSha256, 256, state, null);
            EqualImage(fake.Bytes, target, "Execution used mutable supplied plan");
            Check(state.ResetAllowed, "Snapshot execution state");
        });
        Test("no-op final-image corruption prohibits reset", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, source);
            FakeFlash fake = new FakeFlash(source); fake.CorruptUntouchedTailOnTargetVerify = true;
            ExecutionState state = new ExecutionState();
            FlashTransitionException error = Fails(fake, plan, plan.PlanSha256, state, null);
            Check(state.SourceVerified && fake.FullReads == 2, "No-op did not reach final image verification");
            Check(fake.Writes.Count == 0 && fake.DisableCalls == 0 && !state.MutationStarted, "No-op failure wrote flash");
            Check(!state.ResetAllowed && !state.TargetVerified && !state.Completed, "No-op final-image failure allowed reset");
            Check(!state.RollbackAttempted && error.RollbackStatus == RollbackStatus.NotRequired, "No-op attempted rollback");
        });
        Test("no-op final-status failure prohibits reset", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, source);
            FakeFlash fake = new FakeFlash(source); fake.FailStatusCall = 3;
            ExecutionState state = new ExecutionState();
            FlashTransitionException error = Fails(fake, plan, plan.PlanSha256, state, null);
            Check(state.SourceVerified && fake.FullReads == 2 && fake.StatusReads == 3, "No-op did not reach final status verification");
            Check(fake.Writes.Count == 0 && fake.DisableCalls == 0 && !state.MutationStarted, "No-op status failure wrote flash");
            Check(!state.ResetAllowed && !state.TargetVerified && !state.Completed, "No-op final-status failure allowed reset");
            Check(!state.RollbackAttempted && error.RollbackStatus == RollbackStatus.NotRequired, "No-op attempted rollback");
        });
        Test("identical images perform no writes and summary has no image arrays", delegate {
            byte[] source = Source(); FlashPlan plan = Make(source, source);
            FakeFlash fake = new FakeFlash(source); ExecutionState state = new ExecutionState();
            FlashTransition.Execute(fake, plan, plan.PlanSha256, 256, state, null);
            Check(fake.Writes.Count == 0 && fake.DisableCalls == 0 && state.ResetAllowed && state.Completed, "No-op mutation");
            PlanSummary summary = plan.ToSummary();
            Check(summary.ChangedSectors.Length == 0 && typeof(PlanSummary).GetProperty("SourceFull") == null, "Summary includes images");
        });
        Console.WriteLine("PASS: " + passed + " offline tests; no hardware accessed");
    }
}
}
'@

Add-Type -TypeDefinition ($engine + [Environment]::NewLine + $tests) -Language CSharp
[SamsungExperimentalTests.FlashTransitionOfflineTests]::Run()
