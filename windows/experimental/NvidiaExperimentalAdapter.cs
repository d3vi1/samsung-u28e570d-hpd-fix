using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using SamsungExperimental;

namespace SamsungExperimental
{
    public sealed class ExperimentalRunResult
    {
        public string Direction { get; internal set; }
        public string EdidSerial { get; internal set; }
        public bool TransitionSucceeded { get; internal set; }
        public bool IspExitCompleted { get; internal set; }
        public bool PostBootHealthy { get; internal set; }
        public string Error { get; internal set; }
        public ExecutionState State { get; internal set; }
        public List<string> PersistenceErrors { get; private set; }
        public ExperimentalRunResult() { PersistenceErrors = new List<string>(); }
    }
}

// Separate experimental adapter. The original official updater remains unchanged.
public sealed partial class NvidiaMstarRead
{
    public static ExperimentalRunResult RunExperimental10050(
        PreparedExperiment prepared, bool rollback, string outputDirectory)
    {
        // This check precedes construction: no NVAPI binding or I2C occurs while disabled.
        FlashPlan plan = ExperimentalPolicy.ApprovedExecutionPlan(prepared, rollback);
        string planPin = rollback ? ExperimentalPolicy.ApprovedReversePlanSha256 : ExperimentalPolicy.ApprovedForwardPlanSha256;
        var result = new ExperimentalRunResult {
            Direction = rollback ? "1005.0 to 1004.5" : "1004.5 to 1005.0",
            EdidSerial = ExperimentalPolicy.FirstEdidSerial,
            State = new ExecutionState()
        };
        if (String.IsNullOrWhiteSpace(outputDirectory)) throw new ArgumentException("A unique output directory is required.");
        outputDirectory = Path.GetFullPath(outputDirectory);
        if (Directory.Exists(outputDirectory) && Directory.EnumerateFileSystemEntries(outputDirectory).Any())
            throw new IOException("The output directory must be new or empty.");
        prepared.SaveImages(outputDirectory);

        using (var mutex = new Mutex(false, @"Global\Samsung-MSTAR-Experimental-" + ExperimentalPolicy.FirstEdidSerial))
        {
            var journalFile = new FileStream(Path.Combine(outputDirectory, "transition.log"), FileMode.CreateNew, FileAccess.Write, FileShare.Read);
            var journal = new StreamWriter(journalFile) { AutoFlush = true };
            bool ownsMutex = false;
            bool entered = false;
            bool recoveryMarkerCreated = false;
            string recoveryMarker = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "SamsungMstarExperimental", ExperimentalPolicy.FirstEdidSerial + "-recovery.txt");
            uint previousExecutionState = 0;
            NvidiaMstarRead owner = null;
            Action<string> log = message => {
                string line = DateTimeOffset.UtcNow.ToString("O") + " " + message;
                try { Console.WriteLine(line); } catch { }
                try { journal.WriteLine(line); journalFile.Flush(true); }
                catch (Exception error) { result.PersistenceErrors.Add("Journal: " + error.Message); }
            };
            try
            {
                try { ownsMutex = mutex.WaitOne(0); }
                catch (AbandonedMutexException)
                {
                    ownsMutex = true;
                    throw new InvalidOperationException("An earlier monitor operation terminated unexpectedly. Hardware recovery state must be reviewed before another run.");
                }
                if (!ownsMutex) throw new InvalidOperationException("Another experimental operation owns this monitor.");
                if (File.Exists(recoveryMarker))
                    throw new InvalidOperationException("A persistent recovery marker exists. Review the prior run before accessing hardware: " + recoveryMarker);
                Directory.CreateDirectory(Path.GetDirectoryName(recoveryMarker));
                SaveExperimentalBytes(recoveryMarker, Encoding.UTF8.GetBytes(
                    "Unresolved experimental session\nRun: " + outputDirectory + "\nPlan: " + planPin +
                    "\nStarted: " + DateTimeOffset.UtcNow.ToString("O") + "\n"));
                recoveryMarkerCreated = true;
                log("Direction: " + result.Direction + "; reviewed plan: " + planPin);
                previousExecutionState = SetThreadExecutionState(0x80000003);
                if (previousExecutionState == 0) throw new InvalidOperationException("Could not inhibit sleep during the transition.");
                owner = new NvidiaMstarRead(ExperimentalPolicy.FirstEdidSerial);
                if (owner.EdidSerial != ExperimentalPolicy.FirstEdidSerial)
                    throw new InvalidOperationException("Unexpected monitor identity.");
                owner.VerifyTransport();
                owner.CheckDdc();
                SaveExperimentalBytes(Path.Combine(outputDirectory, "edid-before.bin"), owner.ReadLiveEdid());
                SaveExperimentalSettings(Path.Combine(outputDirectory, "settings-before.txt"), owner.CaptureSettings());
                // Mark an attempted entry before the transaction; an error may follow a side effect.
                entered = true;
                owner.BridgeWrite(Encoding.ASCII.GetBytes("MSTAR"));
                Thread.Sleep(100);
                owner.VerifyExperimentalFlashProfile();

                int pageSize = 32;
                var longReadCommand = new byte[260];
                longReadCommand[0] = 0x03;
                byte[] reference = rollback ? prepared.TargetSnapshot : prepared.BaselineSnapshot;
                if (owner.SpiRead(longReadCommand, 16).SequenceEqual(reference.Skip(256).Take(16))) pageSize = 256;
                log("Read-only transport probe selected " + pageSize + "-byte programming pages.");

                var device = new ExperimentalNvDevice(owner, plan, outputDirectory, result, log);
                FlashTransition.Execute(device, plan, planPin, pageSize, result.State, log);
                result.TransitionSucceeded = true;
            }
            catch (Exception error)
            {
                result.Error = error.ToString();
                log("Transition stopped: " + error.Message);
            }
            finally
            {
                try
                {
                    if (entered && result.State.ResetAllowed)
                    {
                        try
                        {
                            owner.SpiWriteCommand(new byte[] { 0x04 });
                        }
                        catch (Exception error)
                        {
                            result.Error = (result.Error ?? "") + "\nWrite-disable: " + error;
                            log("Write-disable could not be confirmed: " + error.Message);
                        }
                        try
                        {
                            owner.BridgeWrite(new byte[] { 0x24 });
                            result.IspExitCompleted = true;
                            log("ISP exit/reset sent after a verified image or before any mutation.");
                        }
                        catch (Exception error)
                        {
                            result.Error = (result.Error ?? "") + "\nISP exit: " + error;
                            log("ISP exit could not be confirmed: " + error.Message);
                        }
                    }
                    else if (entered)
                        log("RECOVERY REQUIRED: reset withheld because image restoration is unverified.");

                    if (result.IspExitCompleted)
                    {
                        Thread.Sleep(3000);
                        try
                        {
                            SaveExperimentalBytes(Path.Combine(outputDirectory, "edid-after.bin"), owner.ReadLiveEdid());
                            owner.CheckDdc();
                            SaveExperimentalSettings(Path.Combine(outputDirectory, "settings-after.txt"), owner.CaptureSettings());
                            result.PostBootHealthy = true;
                        }
                        catch (Exception error)
                        {
                            result.Error = (result.Error ?? "") + "\nPost-boot check: " + error;
                            log("Post-boot EDID/DDC check failed: " + error.Message);
                        }
                    }
                }
                finally
                {
                    if (previousExecutionState != 0) SetThreadExecutionState(previousExecutionState);
                    if (recoveryMarkerCreated && (!entered ||
                        (result.IspExitCompleted && result.State.ResetAllowed &&
                        (result.State.SourceVerified || result.State.TargetVerified || result.State.RollbackVerified))))
                    {
                        try { File.Delete(recoveryMarker); }
                        catch (Exception error) { result.PersistenceErrors.Add("Recovery marker retained: " + error.Message); }
                    }
                    if (ownsMutex)
                    {
                        try { mutex.ReleaseMutex(); }
                        catch (Exception error) { result.PersistenceErrors.Add("Mutex release: " + error.Message); }
                    }
                    SafeExperimentalDispose(journal, result, "Journal close");
                    SafeExperimentalDispose(journalFile, result, "Journal file close");
                    try
                    {
                        string summary = "Direction: " + result.Direction + "\nSerial: " + result.EdidSerial +
                            "\nTransition succeeded: " + result.TransitionSucceeded + "\nReset allowed: " + result.State.ResetAllowed +
                            "\nISP exit completed: " + result.IspExitCompleted + "\nPost-boot healthy: " + result.PostBootHealthy +
                            "\nPersistence errors: " + String.Join(" | ", result.PersistenceErrors) + "\nError: " + (result.Error ?? "none") + "\n";
                        File.WriteAllText(Path.Combine(outputDirectory, "result.txt"), summary);
                    }
                    catch (Exception error) { result.PersistenceErrors.Add("Final summary: " + error.Message); }
                }
            }
        }
        return result;
    }

    private static void SafeExperimentalDispose(IDisposable resource, ExperimentalRunResult result, string label)
    {
        try { resource.Dispose(); }
        catch (Exception error) { result.PersistenceErrors.Add(label + ": " + error.Message); }
    }

    private void VerifyExperimentalFlashProfile()
    {
        if (EdidSerial != ExperimentalPolicy.FirstEdidSerial) throw new InvalidOperationException("Only the first reviewed monitor is enabled.");
        byte[] expectedId = { 0xef, 0x40, 0x15 };
        if (!SpiRead(new byte[] { 0x9f }, 3).SequenceEqual(expectedId) ||
            !SpiRead(new byte[] { 0x9f }, 3).SequenceEqual(expectedId)) throw new IOException("Flash JEDEC identity mismatch.");
        if (WaitReady(5000) != 0 || SpiRead(new byte[] { 0x35 }, 1)[0] != 2 || SpiRead(new byte[] { 0x15 }, 1)[0] != 0x60)
            throw new IOException("Flash status registers differ from the reviewed profile.");
        byte[] sfdp = { 0x53, 0x46, 0x44, 0x50, 5, 1, 0, 0xff, 0, 5, 1, 16, 0x80, 0, 0, 0xff };
        if (!SpiRead(new byte[] { 0x5a, 0, 0, 0, 0 }, 16).SequenceEqual(sfdp)) throw new IOException("Flash SFDP profile mismatch.");
    }

    private static void SaveExperimentalBytes(string path, byte[] bytes)
    {
        using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
        { file.Write(bytes, 0, bytes.Length); file.Flush(true); }
    }

    private static void SaveExperimentalSettings(string path, Dictionary<string, string> settings)
    {
        using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
        using (var writer = new StreamWriter(file))
        {
            foreach (var pair in settings.OrderBy(p => p.Key)) writer.WriteLine(pair.Key + "=" + pair.Value);
            writer.Flush(); file.Flush(true);
        }
    }

    private sealed class ExperimentalNvDevice : IFlashDevice
    {
        private readonly NvidiaMstarRead owner;
        private readonly HashSet<int> sectorAddresses;
        private readonly string directory;
        private readonly ExperimentalRunResult result;
        private readonly Action<string> log;
        private int fullReadCount;
        public ExperimentalNvDevice(NvidiaMstarRead owner, FlashPlan plan, string directory, ExperimentalRunResult result, Action<string> log)
        {
            this.owner = owner; this.directory = directory; this.result = result; this.log = log;
            sectorAddresses = new HashSet<int>(plan.ChangedSectors.Select(s => s.Address));
        }
        public byte[] ReadRange(int address, int length)
        {
            if (address < 0 || length < 0 || (long)address + length > ExperimentalPolicy.FlashBytes)
                throw new ArgumentOutOfRangeException("address");
            byte[] data = owner.ReadRange(address, length, "Experimental read", log);
            if (address == 0 && length == ExperimentalPolicy.FlashBytes)
            {
                fullReadCount++;
                try { SaveExperimentalBytes(Path.Combine(directory, "full-read-" + fullReadCount.ToString("D2") + ".bin"), data); }
                catch (Exception error) { result.PersistenceErrors.Add("Read-back artifact: " + error.Message); }
            }
            return data;
        }
        public void WriteSector(int address, byte[] fullImage, int pageSize, Action<string> operationLog)
        {
            if (fullImage == null || fullImage.Length != ExperimentalPolicy.FlashBytes ||
                address < 0 || address % 4096 != 0 || address > fullImage.Length - 4096 ||
                !sectorAddresses.Contains(address) || (pageSize != 32 && pageSize != 256))
                throw new InvalidOperationException("Sector operation is outside the reviewed plan.");
            owner.WriteSector(address, fullImage, pageSize, operationLog);
        }
        public byte[] ReadStatus()
        {
            return new byte[] { owner.WaitReady(5000), owner.SpiRead(new byte[] { 0x35 }, 1)[0], owner.SpiRead(new byte[] { 0x15 }, 1)[0] };
        }
        public void WriteDisable() { owner.SpiWriteCommand(new byte[] { 0x04 }); }
    }
}
