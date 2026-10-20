using System.Text;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Reads an ONNX file's <c>metadata_props</c> WITHOUT loading the graph: the top-level
/// <c>ModelProto</c> fields are walked and every length-delimited one other than
/// <c>metadata_props</c> (field 14) is seeked over, so a 340 MB model costs a few reads.
/// <para>
/// <b>Why it exists.</b> Session options must be chosen BEFORE the session is built, and
/// one of them depends on the model: DirectML's graph fusion corrupts some exports (#151:
/// htdemucs; the 62-way Concat of BS-RoFormer's per-band mask estimators) and is verified
/// correct on others (<see cref="OnnxSessionFactory.DirectMlGraphFusionKey"/>). ONNX Runtime
/// can only read metadata from a built session, which for these models takes seconds and
/// gigabytes.
/// </para>
/// </summary>
internal static class OnnxModelMetadata
{
    private const int MetadataPropsField = 14;

    /// <summary>The model's metadata, or an empty map when the file is not a readable
    /// ONNX model. Never throws for a malformed file: no metadata is the safe answer.</summary>
    /// <param name="path">The <c>.onnx</c> file.</param>
    /// <returns>Key to value; later duplicates win.</returns>
    public static IReadOnlyDictionary<string, string> Read(string path)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 4096,
                FileOptions.RandomAccess);
            var end = stream.Length;
            while (stream.Position < end)
            {
                var tag = ReadVarint(stream);
                var field = (int)(tag >> 3);
                switch ((int)(tag & 7))
                {
                    case 0:
                        ReadVarint(stream);
                        break;
                    case 1:
                        stream.Seek(8, SeekOrigin.Current);
                        break;
                    case 5:
                        stream.Seek(4, SeekOrigin.Current);
                        break;
                    case 2:
                    {
                        var length = checked((long)ReadVarint(stream));
                        if (length < 0 || stream.Position + length > end) return result;
                        if (field == MetadataPropsField && length <= 1 << 20)
                        {
                            var entry = new byte[length];
                            stream.ReadExactly(entry);
                            var (key, value) = ParseEntry(entry);
                            if (key != null) result[key] = value ?? "";
                        }
                        else
                        {
                            stream.Seek(length, SeekOrigin.Current);
                        }
                        break;
                    }
                    default:
                        return result;
                }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or OverflowException
                                       or InvalidDataException or ArgumentException)
        {
            // Unreadable or malformed: no metadata.
        }
        return result;
    }

    private static (string? Key, string? Value) ParseEntry(byte[] entry)
    {
        string? key = null, value = null;
        var pos = 0;
        while (pos < entry.Length)
        {
            var tag = ReadVarint(entry, ref pos);
            if ((tag & 7) != 2) return (null, null);
            var length = (int)ReadVarint(entry, ref pos);
            if (length < 0 || pos + length > entry.Length) return (null, null);
            var text = Encoding.UTF8.GetString(entry, pos, length);
            pos += length;
            if (tag >> 3 == 1) key = text;
            else if (tag >> 3 == 2) value = text;
        }
        return (key, value);
    }

    private static ulong ReadVarint(Stream stream)
    {
        ulong value = 0;
        for (var shift = 0; shift < 64; shift += 7)
        {
            var b = stream.ReadByte();
            if (b < 0) throw new InvalidDataException("Truncated varint.");
            value |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return value;
        }
        throw new InvalidDataException("Varint longer than 64 bits.");
    }

    private static ulong ReadVarint(byte[] data, ref int pos)
    {
        ulong value = 0;
        for (var shift = 0; shift < 64 && pos < data.Length; shift += 7)
        {
            var b = data[pos++];
            value |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return value;
        }
        throw new InvalidDataException("Truncated varint.");
    }
}
