using System;
using System.IO;
using System.Security.Cryptography;

namespace SamsungExperimental
{
    // This file prepares immutable image snapshots. It has no hardware dependencies.
    public static class ExperimentalPolicy
    {
        public const int FlashBytes = 2097152;
        public const string FirstEdidSerial = "LOCAL_PROFILE_REQUIRED";
        public const string BaselineSha256 = "b6c42f95c98f14f858824c17775d1a239bb512d1d6dbdaa9a516e1f3854a78c3";
        public const string OfficialPackageSha256 = "60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b";
        public const int CandidatePackageBytes = 581028;
        public const string CandidatePackageSha256 = "3d5cda5eb49c6c0bb0378742aa1767becd31d3b3e85a6c0b9c023bd6ada2dac4";
        public const string CandidateExpandedSha256 = "a4d6791e80e9050f4800e3fcf9fa2fd1af0f84dac0d99ee0b18f2dc17712d3b0";

        // Repository laboratory profile: hardware execution is disabled.
        // Retained artifact pins do not establish hardware qualification for a local device.
        // An editable manifest, command-line switch or environment variable cannot enable this.
        public static readonly bool HardwareEnabled = false;
        public static readonly string ApprovedTargetSha256 = "570adc6fda032365587a183450887bdfb6bbe967afbd31648d472e4fc8b3de75";
        public static readonly string ApprovedForwardPlanSha256 = "b68efbbc4d4ec90d51022d931a82989c5a67860150876237fcd36ba91bd54b9b";
        public static readonly string ApprovedReversePlanSha256 = "7e237ea2b30d1366b71320f1aab4984ba4c3c8783e376c20739b28fce79c83ae";

        public static string Sha256(byte[] data)
        {
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(data)).Replace("-", "").ToLowerInvariant();
        }

        public static void RequireHardwareApproval(PreparedExperiment prepared)
        {
            if (!HardwareEnabled || FirstEdidSerial == "LOCAL_PROFILE_REQUIRED" || ApprovedTargetSha256.Length != 64 ||
                ApprovedForwardPlanSha256.Length != 64 || ApprovedReversePlanSha256.Length != 64)
                throw new InvalidOperationException("Experimental hardware execution is disabled: reviewed image and transition pins are not installed.");
            ValidatePreparedPlans(prepared);
        }

        public static FlashPlan[] ValidatePreparedPlans(PreparedExperiment prepared)
        {
            if (prepared == null || prepared.TargetSha256 != ApprovedTargetSha256 ||
                Sha256(prepared.BaselineSnapshot) != BaselineSha256 ||
                Sha256(prepared.TargetSnapshot) != ApprovedTargetSha256 ||
                Sha256(prepared.PackageSnapshot) != CandidatePackageSha256)
                throw new InvalidOperationException("Prepared images do not match the independently reviewed hardware profile.");
            FlashPlan forward = FlashTransition.ValidateAndSnapshot(prepared.ForwardPlan, ApprovedForwardPlanSha256);
            FlashPlan reverse = FlashTransition.ValidateAndSnapshot(prepared.ReversePlan, ApprovedReversePlanSha256);
            if (forward.SourceSha256 != BaselineSha256 || forward.TargetSha256 != ApprovedTargetSha256 ||
                reverse.SourceSha256 != ApprovedTargetSha256 || reverse.TargetSha256 != BaselineSha256)
                throw new InvalidOperationException("Transition direction does not match the reviewed full images.");
            return new FlashPlan[] { forward, reverse };
        }

        internal static FlashPlan ApprovedExecutionPlan(PreparedExperiment prepared, bool rollback)
        {
            RequireHardwareApproval(prepared);
            return ValidatePreparedPlans(prepared)[rollback ? 1 : 0];
        }
    }

    public sealed class PreparedExperiment
    {
        public FirmwareValidationReport PackageReport { get; internal set; }
        public FlashPlan ForwardPlan { get; internal set; }
        public FlashPlan ReversePlan { get; internal set; }
        public string TargetSha256 { get; internal set; }
        internal byte[] BaselineSnapshot;
        internal byte[] TargetSnapshot;
        internal byte[] PackageSnapshot;

        public void SaveImages(string directory)
        {
            if (String.IsNullOrWhiteSpace(directory)) throw new ArgumentException("An output directory is required.");
            Directory.CreateDirectory(directory);
            SaveNew(Path.Combine(directory, "baseline-1004.5-full.bin"), BaselineSnapshot);
            SaveNew(Path.Combine(directory, "expected-1005.0-full.bin"), TargetSnapshot);
            SaveNew(Path.Combine(directory, "candidate-package.bin"), PackageSnapshot);
        }

        private static void SaveNew(string path, byte[] data)
        {
            using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
            {
                file.Write(data, 0, data.Length);
                file.Flush(true);
            }
        }
    }

    public static class ExperimentPreparation
    {
        public static PreparedExperiment PrepareFiles(string baselinePath, string officialPath, string candidatePath)
        {
            return Prepare(File.ReadAllBytes(baselinePath), File.ReadAllBytes(officialPath), File.ReadAllBytes(candidatePath));
        }

        public static PreparedExperiment Prepare(byte[] baseline, byte[] official, byte[] candidate)
        {
            baseline = baseline == null ? null : (byte[])baseline.Clone();
            official = official == null ? null : (byte[])official.Clone();
            candidate = candidate == null ? null : (byte[])candidate.Clone();
            if (baseline == null || baseline.Length != ExperimentalPolicy.FlashBytes ||
                ExperimentalPolicy.Sha256(baseline) != ExperimentalPolicy.BaselineSha256)
                throw new InvalidDataException("Baseline must be the exact verified 1004.5 full SPI image.");
            if (official == null || official.Length != 578748 ||
                ExperimentalPolicy.Sha256(official) != ExperimentalPolicy.OfficialPackageSha256)
                throw new InvalidDataException("Official source package identity mismatch.");
            if (candidate == null || candidate.Length != ExperimentalPolicy.CandidatePackageBytes ||
                ExperimentalPolicy.Sha256(candidate) != ExperimentalPolicy.CandidatePackageSha256)
                throw new InvalidDataException("Candidate package identity mismatch.");

            // Identity checks and all subsequent work use the same private snapshots.
            byte[] source = baseline;
            byte[] package = candidate;
            FirmwareValidationReport validation = FirmwarePackage.Validate(package, official);
            if (!validation.IsValid)
                throw new InvalidDataException("Candidate format validation failed: " + String.Join("; ", validation.Errors));
            if (validation.MainHeadDifferencesExcludingLength != 0 || validation.ExpandedLength != 1276176 ||
                validation.ExpandedSha256 != ExperimentalPolicy.CandidateExpandedSha256 ||
                validation.Version != "m-E5728UMDB-1005.0")
                throw new InvalidDataException("Candidate body, loader preservation or version differs from the reviewed experiment.");
            byte[] target = (byte[])source.Clone();
            Array.Copy(package, 0, target, 0, package.Length);
            string targetHash = ExperimentalPolicy.Sha256(target);
            int mainSectorEnd = checked(((Math.Max(official.Length, package.Length) + 4095) / 4096) * 4096);
            var ranges = new AddressRange[] {
                new AddressRange(0x10000, 0x11000),
                new AddressRange(0x20000, mainSectorEnd)
            };
            return new PreparedExperiment {
                BaselineSnapshot = source,
                TargetSnapshot = target,
                PackageSnapshot = package,
                PackageReport = validation,
                TargetSha256 = targetHash,
                ForwardPlan = FlashTransition.Plan(source, target, ExperimentalPolicy.BaselineSha256, targetHash, ranges),
                ReversePlan = FlashTransition.Plan(target, source, targetHash, ExperimentalPolicy.BaselineSha256, ranges)
            };
        }
    }
}
