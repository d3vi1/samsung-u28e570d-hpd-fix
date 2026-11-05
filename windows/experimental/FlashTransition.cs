using System;
using System.Collections.Generic;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

// Pure planning and transaction logic. This file has no hardware imports,
// monitor discovery, ISP lifecycle, reset, file writes, or firmware policy.
namespace SamsungExperimental
{
    public interface IFlashDevice
    {
        byte[] ReadRange(int address, int length);
        void WriteSector(int address, byte[] fullImage, int pageSize, Action<string> log);
        byte[] ReadStatus();
        void WriteDisable();
    }

    public sealed class AddressRange
    {
        public int Start { get; private set; }
        public int EndExclusive { get; private set; }
        public AddressRange(int start, int endExclusive)
        {
            Start = start;
            EndExclusive = endExclusive;
        }
    }

    public sealed class SectorChange
    {
        public int Address { get; set; }
        public string BeforeSha256 { get; set; }
        public string AfterSha256 { get; set; }
    }

    public sealed class PlanSummary
    {
        public string Schema { get; set; }
        public int Capacity { get; set; }
        public int SectorSize { get; set; }
        public string SourceSha256 { get; set; }
        public string TargetSha256 { get; set; }
        public string PlanSha256 { get; set; }
        public AddressRange[] ApprovedRanges { get; set; }
        public SectorChange[] ChangedSectors { get; set; }
    }

    // This DTO may be serialized/inspected by a caller. Execute does not trust
    // its mutable fields: it takes copies, rebuilds it, and checks the external pin.
    public sealed class FlashPlan
    {
        public byte[] SourceFull { get; set; }
        public byte[] TargetFull { get; set; }
        public string SourceSha256 { get; set; }
        public string TargetSha256 { get; set; }
        public string PlanSha256 { get; set; }
        public AddressRange[] ApprovedRanges { get; set; }
        public SectorChange[] ChangedSectors { get; set; }

        public PlanSummary ToSummary()
        {
            return new PlanSummary {
                Schema = FlashTransition.Schema,
                Capacity = FlashTransition.Capacity,
                SectorSize = FlashTransition.SectorSize,
                SourceSha256 = SourceSha256,
                TargetSha256 = TargetSha256,
                PlanSha256 = PlanSha256,
                ApprovedRanges = FlashTransition.CopyRanges(ApprovedRanges),
                ChangedSectors = FlashTransition.CopySectors(ChangedSectors)
            };
        }
    }

    public enum RollbackStatus { NotRequired, Succeeded, Failed }

    public sealed class ExecutionState
    {
        private readonly List<int> touched = new List<int>();
        public bool Started { get; internal set; }
        public bool ResetAllowed { get; internal set; }
        public bool SourceVerified { get; internal set; }
        public bool MutationStarted { get; internal set; }
        public bool TargetVerified { get; internal set; }
        public bool Completed { get; internal set; }
        public bool RollbackAttempted { get; internal set; }
        public bool RollbackVerified { get; internal set; }
        public RollbackStatus RollbackStatus { get; internal set; }
        public string LastError { get; internal set; }
        public int[] TouchedSectors { get { return touched.ToArray(); } }

        public ExecutionState()
        {
            // A previous interrupted operation may have left a mixed image.
            // No reset is permitted until this invocation verifies a full image.
            ResetAllowed = false;
            RollbackStatus = RollbackStatus.NotRequired;
        }
        internal void Touch(int address) { touched.Add(address); }
    }

    public sealed class FlashTransitionException : Exception
    {
        public ExecutionState State { get; private set; }
        public RollbackStatus RollbackStatus { get; private set; }
        public Exception RollbackError { get; private set; }
        public FlashTransitionException(string message, Exception error,
            ExecutionState state, Exception rollbackError) : base(message, error)
        {
            State = state;
            RollbackStatus = state.RollbackStatus;
            RollbackError = rollbackError;
        }
    }

    public static class FlashTransition
    {
        public const int Capacity = 2097152;
        public const int SectorSize = 4096;
        public const int HeaderSector = 0x10000;
        public const string Schema = "SamsungExperimental.FlashPlan/v1";

        public static string Sha256(byte[] bytes)
        {
            if (bytes == null) throw new ArgumentNullException("bytes");
            using (SHA256 sha = SHA256.Create())
                return Hex(sha.ComputeHash(bytes));
        }
        private static string Hex(byte[] bytes)
        {
            return BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
        }
        private static string SectorHash(byte[] image, int address)
        {
            using (SHA256 sha = SHA256.Create())
                return Hex(sha.ComputeHash(image, address, SectorSize));
        }
        private static string NormalizeHash(string hash, string name)
        {
            if (hash == null || hash.Length != 64) throw new ArgumentException("Invalid SHA-256: " + name);
            for (int i = 0; i < hash.Length; i++)
                if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f')
                    || (hash[i] >= 'A' && hash[i] <= 'F')))
                    throw new ArgumentException("Invalid SHA-256: " + name);
            return hash.ToLowerInvariant();
        }
        private static byte[] CopyImage(byte[] image, string name)
        {
            if (image == null || image.Length != Capacity)
                throw new ArgumentException(name + " must be exactly 2097152 bytes");
            return (byte[])image.Clone();
        }
        internal static AddressRange[] CopyRanges(AddressRange[] ranges)
        {
            if (ranges == null) throw new ArgumentNullException("approvedRanges");
            AddressRange[] result = new AddressRange[ranges.Length];
            for (int i = 0; i < ranges.Length; i++)
            {
                if (ranges[i] == null) throw new ArgumentException("Null approved range");
                result[i] = new AddressRange(ranges[i].Start, ranges[i].EndExclusive);
            }
            return result;
        }
        internal static SectorChange[] CopySectors(SectorChange[] sectors)
        {
            if (sectors == null) throw new ArgumentNullException("changedSectors");
            SectorChange[] result = new SectorChange[sectors.Length];
            for (int i = 0; i < sectors.Length; i++)
            {
                if (sectors[i] == null) throw new ArgumentException("Null sector entry");
                result[i] = new SectorChange { Address = sectors[i].Address,
                    BeforeSha256 = sectors[i].BeforeSha256, AfterSha256 = sectors[i].AfterSha256 };
            }
            return result;
        }
        private static AddressRange[] NormalizeRanges(AddressRange[] ranges)
        {
            AddressRange[] result = CopyRanges(ranges);
            Array.Sort(result, delegate(AddressRange a, AddressRange b) { return a.Start.CompareTo(b.Start); });
            for (int i = 0; i < result.Length; i++)
            {
                if (result[i].Start < 0 || result[i].EndExclusive > Capacity
                    || result[i].EndExclusive <= result[i].Start)
                    throw new ArgumentException("Approved range is outside the flash or empty");
                if (i > 0 && result[i].Start < result[i - 1].EndExclusive)
                    throw new ArgumentException("Approved ranges overlap");
            }
            return result;
        }
        private static bool IsAllowed(int address, AddressRange[] ranges)
        {
            for (int i = 0; i < ranges.Length; i++)
                if (address >= ranges[i].Start && address < ranges[i].EndExclusive) return true;
            return false;
        }
        private static string Number(int value) { return value.ToString(CultureInfo.InvariantCulture); }
        private static string ComputePlanHash(FlashPlan plan)
        {
            StringBuilder text = new StringBuilder();
            text.Append(Schema).Append('\n').Append(Number(Capacity)).Append('\n')
                .Append(Number(SectorSize)).Append('\n').Append(plan.SourceSha256).Append('\n')
                .Append(plan.TargetSha256).Append('\n');
            foreach (AddressRange range in plan.ApprovedRanges)
                text.Append("range:").Append(Number(range.Start)).Append(':')
                    .Append(Number(range.EndExclusive)).Append('\n');
            foreach (SectorChange sector in plan.ChangedSectors)
                text.Append("sector:").Append(Number(sector.Address)).Append(':')
                    .Append(sector.BeforeSha256).Append(':').Append(sector.AfterSha256).Append('\n');
            return Sha256(Encoding.UTF8.GetBytes(text.ToString()));
        }

        public static FlashPlan Plan(byte[] sourceFull, byte[] targetFull,
            string pinnedSourceSha256, string pinnedTargetSha256, AddressRange[] approvedRanges)
        {
            byte[] source = CopyImage(sourceFull, "Source image");
            byte[] target = CopyImage(targetFull, "Target image");
            string sourceHash = NormalizeHash(pinnedSourceSha256, "source");
            string targetHash = NormalizeHash(pinnedTargetSha256, "target");
            if (Sha256(source) != sourceHash) throw new ArgumentException("Source image hash mismatch");
            if (Sha256(target) != targetHash) throw new ArgumentException("Target image hash mismatch");
            AddressRange[] ranges = NormalizeRanges(approvedRanges);
            bool[] changed = new bool[Capacity / SectorSize];
            for (int address = 0; address < Capacity; address++)
            {
                if (source[address] == target[address]) continue;
                if (!IsAllowed(address, ranges))
                    throw new ArgumentException("Changed byte outside approved ranges at 0x" + address.ToString("X6"));
                changed[address / SectorSize] = true;
            }
            List<SectorChange> sectors = new List<SectorChange>();
            SectorChange header = null;
            for (int index = 0; index < changed.Length; index++)
            {
                if (!changed[index]) continue;
                int address = index * SectorSize;
                SectorChange sector = new SectorChange { Address = address,
                    BeforeSha256 = SectorHash(source, address), AfterSha256 = SectorHash(target, address) };
                if (address == HeaderSector) header = sector;
                else sectors.Add(sector);
            }
            if (header != null) sectors.Add(header);
            FlashPlan plan = new FlashPlan { SourceFull = source, TargetFull = target,
                SourceSha256 = sourceHash, TargetSha256 = targetHash, ApprovedRanges = ranges,
                ChangedSectors = sectors.ToArray() };
            plan.PlanSha256 = ComputePlanHash(plan);
            return plan;
        }

        // Pure validation, exposed so adapters can reject changed DTOs before
        // constructing a hardware object. The returned plan owns fresh copies.
        public static FlashPlan ValidateAndSnapshot(FlashPlan supplied, string pinnedPlanSha256)
        {
            if (supplied == null) throw new ArgumentNullException("plan");
            string pin = NormalizeHash(pinnedPlanSha256, "plan pin");
            string declaredHash = NormalizeHash(supplied.PlanSha256, "declared plan");
            SectorChange[] declared = CopySectors(supplied.ChangedSectors);
            FlashPlan actual = Plan(supplied.SourceFull, supplied.TargetFull,
                supplied.SourceSha256, supplied.TargetSha256, supplied.ApprovedRanges);
            if (actual.PlanSha256 != pin || declaredHash != pin)
                throw new ArgumentException("Pinned plan hash mismatch");
            if (declared.Length != actual.ChangedSectors.Length)
                throw new ArgumentException("Declared changed-sector count differs from exact image diff");
            for (int i = 0; i < declared.Length; i++)
            {
                SectorChange expected = actual.ChangedSectors[i];
                if (declared[i].Address != expected.Address
                    || NormalizeHash(declared[i].BeforeSha256, "sector before") != expected.BeforeSha256
                    || NormalizeHash(declared[i].AfterSha256, "sector after") != expected.AfterSha256)
                    throw new ArgumentException("Declared changed sectors differ from exact image diff/order");
            }
            return actual;
        }
        private static bool Equal(byte[] left, byte[] right)
        {
            if (left == null || right == null || left.Length != right.Length) return false;
            for (int i = 0; i < left.Length; i++) if (left[i] != right[i]) return false;
            return true;
        }
        private static byte[] Status(IFlashDevice device)
        {
            byte[] status = device.ReadStatus();
            if (status == null || status.Length == 0) throw new InvalidOperationException("Empty status vector");
            return (byte[])status.Clone();
        }
        private static void VerifyFull(IFlashDevice device, string expectedHash, string label)
        {
            byte[] actual = device.ReadRange(0, Capacity);
            if (actual == null || actual.Length != Capacity || Sha256(actual) != expectedHash)
                throw new InvalidOperationException(label + " full-image verification failed");
        }
        private static void VerifySector(IFlashDevice device, int address, string expectedHash)
        {
            byte[] actual = device.ReadRange(address, SectorSize);
            if (actual == null || actual.Length != SectorSize || Sha256(actual) != expectedHash)
                throw new InvalidOperationException("Sector verification failed at 0x" + address.ToString("X6"));
        }
        private static void SafeLog(Action<string> log, string message)
        {
            if (log == null) return;
            try { log(message); }
            catch (Exception) { /* Diagnostic failures must never interrupt restoration. */ }
        }

        public static void Execute(IFlashDevice device, FlashPlan plan, string pinnedPlanSha256,
            int forwardPageSize, ExecutionState state, Action<string> log)
        {
            if (state == null) throw new ArgumentNullException("state");
            lock (state)
            {
                if (state.Started)
                    throw new FlashTransitionException("ExecutionState cannot be reused", null, state, null);
                state.Started = true;
            }
            FlashPlan snapshot = null;
            byte[] initialStatus = null;
            Action<string> safeLog = delegate(string message) { SafeLog(log, message); };
            try
            {
                if (device == null) throw new ArgumentNullException("device");
                if (forwardPageSize != 32 && forwardPageSize != 256)
                    throw new ArgumentException("Only 32-byte or 256-byte programming is supported");
                snapshot = ValidateAndSnapshot(plan, pinnedPlanSha256);
                initialStatus = Status(device);
                VerifyFull(device, snapshot.SourceSha256, "Preflight source");
                if (!Equal(initialStatus, Status(device)))
                    throw new InvalidOperationException("Status changed during preflight");
                state.SourceVerified = true;
                state.ResetAllowed = true;
                safeLog("Full source image and pinned transition plan verified");
                foreach (SectorChange sector in snapshot.ChangedSectors)
                {
                    state.ResetAllowed = false;
                    state.MutationStarted = true;
                    state.Touch(sector.Address); // A throwing write may already have erased the sector.
                    safeLog("Updating sector 0x" + sector.Address.ToString("X6"));
                    device.WriteSector(sector.Address, (byte[])snapshot.TargetFull.Clone(), forwardPageSize, safeLog);
                    VerifySector(device, sector.Address, sector.AfterSha256);
                }
                if (state.MutationStarted) device.WriteDisable();
                // Revoke preflight permission even when no sectors changed: a
                // failed final image/status check must never retain reset permission.
                state.ResetAllowed = false;
                VerifyFull(device, snapshot.TargetSha256, "Target");
                if (!Equal(initialStatus, Status(device)))
                    throw new InvalidOperationException("Status differs after target verification");
                state.TargetVerified = true;
                state.Completed = true;
                state.ResetAllowed = true;
                safeLog("SUCCESS: exact full target image and status verified");
            }
            catch (Exception error)
            {
                state.LastError = error.Message;
                if (!state.MutationStarted)
                {
                    safeLog("Stopped before flash mutation: " + error.Message);
                    throw new FlashTransitionException("Transition rejected before flash mutation", error, state, null);
                }
                state.ResetAllowed = false;
                state.RollbackAttempted = true;
                state.RollbackStatus = RollbackStatus.Failed;
                safeLog("Transition failed; restoring every touched sector: " + error.Message);
                List<Exception> rollbackErrors = new List<Exception>();
                foreach (int address in state.TouchedSectors)
                {
                    try
                    {
                        device.WriteSector(address, (byte[])snapshot.SourceFull.Clone(), 32, safeLog);
                        VerifySector(device, address, SectorHash(snapshot.SourceFull, address));
                        safeLog("Restored sector 0x" + address.ToString("X6"));
                    }
                    catch (Exception rollbackError)
                    {
                        rollbackErrors.Add(rollbackError);
                        safeLog("Restoration error at 0x" + address.ToString("X6") + ": " + rollbackError.Message);
                    }
                }
                try { device.WriteDisable(); }
                catch (Exception disableError) { rollbackErrors.Add(disableError); }
                try
                {
                    // Final evidence, not individual command return codes, determines
                    // whether resetting can safely boot the original image again.
                    VerifyFull(device, snapshot.SourceSha256, "Rollback source");
                    if (!Equal(initialStatus, Status(device)))
                        throw new InvalidOperationException("Status differs after rollback");
                    state.RollbackVerified = true;
                    state.RollbackStatus = RollbackStatus.Succeeded;
                    state.ResetAllowed = true;
                    safeLog("ROLLBACK VERIFIED: exact full source image and status restored");
                }
                catch (Exception verificationError)
                {
                    rollbackErrors.Add(verificationError);
                    safeLog("RECOVERY REQUIRED: reset remains prohibited");
                }
                Exception rollbackFailure = rollbackErrors.Count == 0 ? null : new AggregateException(rollbackErrors);
                throw new FlashTransitionException(state.RollbackVerified
                    ? "Transition failed; exact source image restored"
                    : "Transition failed; rollback could not be verified; reset prohibited",
                    error, state, rollbackFailure);
            }
        }
    }
}
