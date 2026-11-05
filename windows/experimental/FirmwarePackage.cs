using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

namespace SamsungExperimental
{
    // Offline format validation only. A valid report is not authorization to flash.
    public sealed class FirmwareValidationReport
    {
        public bool IsValid { get { return Errors.Count == 0; } }
        public readonly List<string> Errors = new List<string>();
        public readonly List<string> Warnings = new List<string>();
        public int PackageLength;
        public string PackageSha256;
        public ushort Sum16;
        public uint MainStart, MainEnd, LoadAddress, LoadEnd, PackageCrcOffset;
        public uint MainCrcExpected, MainCrcComputed, PackageCrcExpected, PackageCrcComputed, PackageCrcRemainder;
        public ushort InnerCrcExpected, InnerCrcComputed, OuterCrcExpected, OuterCrcComputed;
        public uint EmbeddedMainLength, EmbeddedHeadLength, ExpandedLengthDeclared;
        public int DeflateStart, DeflateEnd, DeflateConsumedBytes, ExpandedLength;
        public string ExpandedSha256, Version;
        public bool BootPreserved, Pm51Preserved;
        public string BootSha256, Pm51Sha256;
        public int MainHeadDifferencesExcludingLength;
    }

    public static class FirmwarePackage
    {
        public const int MaximumPackageBytes = 0x200000;
        public const int MaximumExpandedBytes = 8 * 1024 * 1024;
        public const int HeaderOffset = 0x10000;
        public const int MainStartOffset = 0x20080;
        public const int DeflateStartOffset = 0x23880;
        public const string OfficialSha256 = "60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b";
        private static readonly uint[] Crc32Table = MakeCrc32Table();
        private static readonly ushort[] Crc16Table = MakeCrc16Table();

        public static FirmwareValidationReport Validate(byte[] package, byte[] officialReference)
        {
            FirmwareValidationReport r = new FirmwareValidationReport();
            if (package == null) { r.Errors.Add("PackageMissing: package is null."); return r; }
            r.PackageLength = package.Length;
            r.PackageSha256 = Sha256Hex(package, 0, package.Length);
            uint sum = 0;
            for (int i = 0; i < package.Length; i++) sum = (sum + package[i]) & 0xFFFF;
            r.Sum16 = (ushort)sum;
            if (package.Length < DeflateStartOffset + 6 + 8 + 4 || package.Length > MaximumPackageBytes)
            {
                r.Errors.Add("PackageLength: outside supported Samsung package bounds.");
                return r;
            }

            bool referenceValid = officialReference != null && officialReference.Length == 578748 &&
                String.Equals(Sha256Hex(officialReference, 0, officialReference.Length), OfficialSha256, StringComparison.Ordinal);
            if (!referenceValid) r.Errors.Add("OfficialReference: exact verified 1004.5 BIN is required.");
            r.BootSha256 = Sha256Hex(package, 0, 0x10000);
            r.Pm51Sha256 = Sha256Hex(package, 0x10080, 0x10000);
            if (referenceValid)
            {
                r.BootPreserved = EqualRange(package, officialReference, 0, 0x10000);
                r.Pm51Preserved = EqualRange(package, officialReference, 0x10080, 0x10000);
                if (!r.BootPreserved) r.Errors.Add("BootChanged: bootstrap differs from official 1004.5.");
                if (!r.Pm51Preserved) r.Errors.Add("Pm51Changed: complete PM51 slot differs from official 1004.5.");
                // Header fields permitted to vary with recompression: load end, main end,
                // AP CRC32, and final package CRC offset. Preserve every other header byte.
                for (int i = 0; i < 0x80; i++)
                {
                    if ((i >= 8 && i < 16) || (i >= 20 && i < 24) || (i >= 32 && i < 36)) continue;
                    if (package[HeaderOffset + i] != officialReference[HeaderOffset + i])
                    {
                        r.Errors.Add("HeaderConstants: an invariant header byte changed.");
                        break;
                    }
                }
                for (int i = MainStartOffset; i < DeflateStartOffset; i++)
                    if (!(i >= 0x211CA && i < 0x211CE) && package[i] != officialReference[i])
                        r.MainHeadDifferencesExcludingLength++;
                if (r.MainHeadDifferencesExcludingLength != 0)
                    r.Warnings.Add("MainHeadChanged: changes beyond the embedded length exist; historical versions differ here, but a body-only patch needs separate exact-diff approval.");
            }

            r.MainStart = ReadLe32(package, 0x10000);
            r.LoadAddress = ReadLe32(package, 0x10004);
            r.LoadEnd = ReadLe32(package, 0x10008);
            r.MainEnd = ReadLe32(package, 0x1000C);
            r.MainCrcExpected = ReadLe32(package, 0x10014);
            r.PackageCrcOffset = ReadLe32(package, 0x10020);
            if (r.MainStart != MainStartOffset || r.MainEnd < DeflateStartOffset + 7 || r.MainEnd > (uint)package.Length - 12)
            {
                r.Errors.Add("MainBounds: invalid fixed start or main end.");
                return r;
            }
            ulong expectedCrcOffset = (((ulong)r.MainEnd + 7UL) & ~7UL) + 8UL;
            if (r.PackageCrcOffset != expectedCrcOffset || (ulong)r.PackageCrcOffset + 4UL != (ulong)package.Length)
            {
                r.Errors.Add("PackageLength: header CRC offset, eight-byte alignment and actual length disagree.");
                return r;
            }
            uint mainLength = r.MainEnd - r.MainStart;
            if (r.LoadAddress != 0x200000 || (ulong)r.LoadAddress + mainLength != r.LoadEnd)
                r.Errors.Add("LoadBounds: load address/end do not describe the packaged main payload.");

            r.EmbeddedMainLength = ReadLe32(package, 0x211CA);
            r.EmbeddedHeadLength = ReadLe32(package, 0x211CE);
            if (package[0x211C7] != 0xBE || package[0x211C8] != 0xAD || package[0x211C9] != 0xDE)
                r.Errors.Add("MainHeadMagic: BE AD DE marker missing.");
            if (r.EmbeddedMainLength != mainLength)
                r.Errors.Add("EmbeddedMainLength: word at 0x211CA does not equal mainEnd-mainStart.");
            if (r.EmbeddedHeadLength != 0x3800)
                r.Errors.Add("EmbeddedHeadLength: word at 0x211CE is not 0x3800.");

            int mainEnd = (int)r.MainEnd;
            int crcOffset = (int)r.PackageCrcOffset;
            int markerOffset = crcOffset - 8;
            for (int i = mainEnd; i < markerOffset; i++)
                if (package[i] != 0) { r.Errors.Add("AlignmentPadding: nonzero byte after main payload."); break; }
            byte[] marker = new byte[] { 0x55, 0xAA, 0xAB, 0xCD, 0x00, 0x00 };
            for (int i = 0; i < marker.Length; i++)
                if (package[markerOffset + i] != marker[i]) { r.Errors.Add("PackageMarker: six-byte marker prefix differs."); break; }

            r.InnerCrcExpected = ReadBe16(package, mainEnd - 2);
            r.InnerCrcComputed = ComputeSamsungCrc16(package, HeaderOffset, mainEnd - 2 - HeaderOffset);
            r.OuterCrcExpected = ReadBe16(package, crcOffset - 2);
            r.OuterCrcComputed = ComputeSamsungCrc16(package, HeaderOffset, crcOffset - 2 - HeaderOffset);
            if (r.InnerCrcExpected != r.InnerCrcComputed) r.Errors.Add("InnerCRC16: post-DEFLATE CRC mismatch.");
            if (r.OuterCrcExpected != r.OuterCrcComputed) r.Errors.Add("OuterCRC16: package marker CRC mismatch.");
            r.MainCrcComputed = ComputeCrc32(package, (int)r.MainStart, (int)mainLength);
            if (r.MainCrcExpected != r.MainCrcComputed) r.Errors.Add("MainCRC32: AP payload CRC mismatch.");
            r.PackageCrcExpected = ReadBe32(package, crcOffset);
            r.PackageCrcComputed = ComputeCrc32(package, 0, crcOffset);
            r.PackageCrcRemainder = ComputeCrc32(package, 0, package.Length);
            if (r.PackageCrcExpected != r.PackageCrcComputed || r.PackageCrcRemainder != 0)
                r.Errors.Add("PackageCRC32: final CRC mismatch or nonzero residue.");

            r.DeflateStart = DeflateStartOffset;
            r.DeflateEnd = mainEnd - 6;
            r.ExpandedLengthDeclared = ReadLe32(package, r.DeflateEnd);
            if (r.ExpandedLengthDeclared == 0 || r.ExpandedLengthDeclared > MaximumExpandedBytes)
            {
                r.Errors.Add("ExpandedSizeLimit: declared size is zero or exceeds 8 MiB.");
                return r;
            }
            try
            {
                using (OneByteReadStream input = new OneByteReadStream(package, r.DeflateStart, r.DeflateEnd - r.DeflateStart))
                using (DeflateStream inflater = new DeflateStream(input, CompressionMode.Decompress, true))
                using (MemoryStream expanded = new MemoryStream())
                {
                    byte[] block = new byte[8192];
                    int got;
                    while ((got = inflater.Read(block, 0, block.Length)) != 0)
                    {
                        if (expanded.Length + got > MaximumExpandedBytes || expanded.Length + got > r.ExpandedLengthDeclared)
                            throw new InvalidDataException("Expanded data exceeds declared size or 8 MiB limit.");
                        expanded.Write(block, 0, got);
                    }
                    r.DeflateConsumedBytes = input.BytesRead;
                    r.ExpandedLength = (int)expanded.Length;
                    // DeflateStream can return zero at underlying EOF without having
                    // seen BFINAL. A complete stream finishes without asking for a
                    // byte beyond its compressed range; reject that extra read.
                    if (input.ReadPastEndAttempted)
                        r.Errors.Add("DeflateTruncated: inflater exhausted input before reporting a complete stream.");
                    if (input.BytesRead != r.DeflateEnd - r.DeflateStart)
                        r.Errors.Add("DeflateBoundary: bytes remain between DEFLATE end and its six-byte trailer.");
                    if (r.ExpandedLength != r.ExpandedLengthDeclared)
                        r.Errors.Add("ExpandedLength: actual size differs from trailer.");
                    byte[] body = expanded.ToArray();
                    r.ExpandedSha256 = Sha256Hex(body, 0, body.Length);
                    r.Version = FindVersion(body);
                }
            }
            catch (InvalidDataException ex) { r.Errors.Add("Deflate: " + ex.Message); }
            catch (IOException ex) { r.Errors.Add("Deflate: " + ex.Message); }
            return r;
        }

        public static string Sha256Hex(byte[] data, int offset, int count)
        {
            CheckRange(data, offset, count);
            using (SHA256 sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(data, offset, count)).Replace("-", "").ToLowerInvariant();
        }

        public static uint ComputeCrc32(byte[] data, int offset, int count)
        {
            CheckRange(data, offset, count);
            uint crc = 0;
            for (int i = offset; i < offset + count; i++)
                crc = (crc << 8) ^ Crc32Table[(int)((crc >> 24) ^ data[i])];
            return crc;
        }

        // Absolute file offsets 0x10014..0x10017 are zero during both CRC16 calculations.
        public static ushort ComputeSamsungCrc16(byte[] data, int offset, int count)
        {
            CheckRange(data, offset, count);
            ushort crc = 0;
            for (int i = offset; i < offset + count; i++)
            {
                byte value = (i >= 0x10014 && i < 0x10018) ? (byte)0 : data[i];
                crc = (ushort)((crc << 8) ^ Crc16Table[(crc >> 8) ^ value]);
            }
            return crc;
        }

        private static uint[] MakeCrc32Table()
        {
            uint[] table = new uint[256];
            for (int i = 0; i < table.Length; i++)
            {
                uint c = (uint)i << 24;
                for (int bit = 0; bit < 8; bit++) c = (c << 1) ^ ((c & 0x80000000) != 0 ? 0x04C11DB7U : 0U);
                table[i] = c;
            }
            return table;
        }

        private static ushort[] MakeCrc16Table()
        {
            ushort[] table = new ushort[256];
            for (int i = 0; i < table.Length; i++)
            {
                ushort c = (ushort)(i << 8);
                for (int bit = 0; bit < 8; bit++) c = (ushort)((c << 1) ^ ((c & 0x8000) != 0 ? 0x8005 : 0));
                table[i] = c;
            }
            return table;
        }

        private static void CheckRange(byte[] data, int offset, int count)
        {
            if (data == null) throw new ArgumentNullException("data");
            if (offset < 0 || count < 0 || offset > data.Length - count) throw new ArgumentOutOfRangeException("offset");
        }
        private static uint ReadLe32(byte[] b, int p) { return (uint)(b[p] | b[p + 1] << 8 | b[p + 2] << 16 | b[p + 3] << 24); }
        private static uint ReadBe32(byte[] b, int p) { return (uint)(b[p] << 24 | b[p + 1] << 16 | b[p + 2] << 8 | b[p + 3]); }
        private static ushort ReadBe16(byte[] b, int p) { return (ushort)(b[p] << 8 | b[p + 1]); }
        private static bool EqualRange(byte[] a, byte[] b, int p, int count)
        {
            for (int i = p; i < p + count; i++) if (a[i] != b[i]) return false;
            return true;
        }
        private static string FindVersion(byte[] body)
        {
            byte[] prefix = Encoding.ASCII.GetBytes("m-E5728UMDB-");
            for (int i = 0; i <= body.Length - prefix.Length; i++)
            {
                int j = 0;
                while (j < prefix.Length && body[i + j] == prefix[j]) j++;
                if (j != prefix.Length) continue;
                int end = i + j;
                while (end < body.Length && end - i < 64 && body[end] >= 32 && body[end] < 127) end++;
                return Encoding.ASCII.GetString(body, i, end - i);
            }
            return null;
        }

        // DeflateStream may otherwise read past BFINAL into a trailer. Limit each
        // underlying read to one byte and expose only the declared compressed range.
        private sealed class OneByteReadStream : Stream
        {
            private readonly byte[] data;
            private readonly int start, count;
            public int BytesRead { get; private set; }
            public bool ReadPastEndAttempted { get; private set; }
            public OneByteReadStream(byte[] data, int start, int count) { this.data = data; this.start = start; this.count = count; }
            public override int Read(byte[] buffer, int offset, int requested)
            {
                if (buffer == null) throw new ArgumentNullException("buffer");
                if (offset < 0 || requested < 0 || offset > buffer.Length - requested) throw new ArgumentOutOfRangeException("offset");
                if (requested == 0) return 0;
                if (BytesRead == count) { ReadPastEndAttempted = true; return 0; }
                buffer[offset] = data[start + BytesRead++];
                return 1;
            }
            public override int ReadByte()
            {
                if (BytesRead == count) { ReadPastEndAttempted = true; return -1; }
                return data[start + BytesRead++];
            }
            public override bool CanRead { get { return true; } }
            public override bool CanSeek { get { return false; } }
            public override bool CanWrite { get { return false; } }
            public override long Length { get { return count; } }
            public override long Position { get { return BytesRead; } set { throw new NotSupportedException(); } }
            public override void Flush() { }
            public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
            public override void SetLength(long value) { throw new NotSupportedException(); }
            public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
        }
    }
}
