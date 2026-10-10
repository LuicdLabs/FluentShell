<#
Lists the dialog templates (RT_DIALOG) a module carries: for each dialog its id, style bits and font, and for each
control its class, id, style, extended style, rectangle (dialog units) and text. Static analysis only: the module
is mapped as data, never run, so it names which control classes and styles a native dialog will need before the
dialog is ever opened. Resources are language-neutral in the module itself or moved to a MUI file; pass the MUI
path (for example C:\Windows\System32\zh-TW\sysmon.ocx.mui) when the module reports none.

  .\tools\Dump-DialogResources.ps1 -Path C:\Windows\System32\sysmon.ocx
  .\tools\Dump-DialogResources.ps1 -Path C:\Windows\System32\sysmon.ocx -Id 201,202
#>
param(
    [Parameter(Mandatory = $true)][string]$Path,
    # Only these dialog ids; empty lists every dialog.
    [int[]]$Id = @()
)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class DialogResources {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    delegate bool EnumNames(IntPtr module, IntPtr type, IntPtr name, IntPtr param);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool EnumResourceNamesW(IntPtr module, IntPtr type, EnumNames callback, IntPtr param);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint SizeofResource(IntPtr module, IntPtr resource);

    const uint LOAD_LIBRARY_AS_DATAFILE = 2;
    const uint LOAD_LIBRARY_AS_IMAGE_RESOURCE = 0x20;
    static readonly IntPtr RT_DIALOG = new IntPtr(5);

    static readonly string[] SystemClasses = { "Button", "Edit", "Static", "ListBox", "ScrollBar", "ComboBox" };

    static string ReadSz(byte[] data, ref int offset) {
        StringBuilder text = new StringBuilder();
        while (offset + 1 < data.Length) {
            char c = (char)(data[offset] | (data[offset + 1] << 8));
            offset += 2;
            if (c == 0) break;
            text.Append(c);
        }
        return text.ToString();
    }

    // A name or ordinal: 0x0000 is none, 0xFFFF is followed by an ordinal, anything else is a string.
    static string ReadNameOrOrdinal(byte[] data, ref int offset) {
        ushort first = (ushort)(data[offset] | (data[offset + 1] << 8));
        if (first == 0) { offset += 2; return ""; }
        if (first == 0xFFFF) {
            ushort ordinal = (ushort)(data[offset + 2] | (data[offset + 3] << 8));
            offset += 4;
            return "#" + ordinal;
        }
        return ReadSz(data, ref offset);
    }

    static void Align(ref int offset, int boundary) { offset = (offset + boundary - 1) & ~(boundary - 1); }

    public static List<string> Dump(string path, int[] only) {
        List<string> lines = new List<string>();
        IntPtr module = LoadLibraryExW(path, IntPtr.Zero, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (module == IntPtr.Zero) { lines.Add("LoadLibraryEx failed " + Marshal.GetLastWin32Error()); return lines; }
        List<IntPtr> names = new List<IntPtr>();
        List<string> labels = new List<string>();
        EnumNames callback = (m, type, name, param) => {
            long value = name.ToInt64();
            names.Add(name);
            labels.Add(value < 0x10000 ? value.ToString() : Marshal.PtrToStringUni(name));
            return true;
        };
        EnumResourceNamesW(module, RT_DIALOG, callback, IntPtr.Zero);
        for (int index = 0; index < names.Count; index++) {
            long ordinal = names[index].ToInt64();
            if (only.Length != 0 && (ordinal >= 0x10000 || Array.IndexOf(only, (int)ordinal) < 0)) continue;
            IntPtr resource = FindResourceW(module, names[index], RT_DIALOG);
            if (resource == IntPtr.Zero) continue;
            uint size = SizeofResource(module, resource);
            IntPtr pointer = LockResource(LoadResource(module, resource));
            byte[] data = new byte[size];
            Marshal.Copy(pointer, data, 0, (int)size);
            Describe(labels[index], data, lines);
        }
        FreeLibrary(module);
        return lines;
    }

    static void Describe(string label, byte[] data, List<string> lines) {
        int offset = 0;
        ushort version = BitConverter.ToUInt16(data, 0), signature = BitConverter.ToUInt16(data, 2);
        bool extended = version == 1 && signature == 0xFFFF;
        uint style, exStyle; int count; short x, y, cx, cy;
        if (extended) {
            offset = 8; // dlgVer, signature, helpID
            exStyle = BitConverter.ToUInt32(data, offset); offset += 4;
            style = BitConverter.ToUInt32(data, offset); offset += 4;
        } else {
            style = BitConverter.ToUInt32(data, 0); exStyle = BitConverter.ToUInt32(data, 4); offset = 8;
        }
        count = BitConverter.ToUInt16(data, offset); offset += 2;
        x = BitConverter.ToInt16(data, offset); y = BitConverter.ToInt16(data, offset + 2);
        cx = BitConverter.ToInt16(data, offset + 4); cy = BitConverter.ToInt16(data, offset + 6); offset += 8;
        string menu = ReadNameOrOrdinal(data, ref offset);
        string dialogClass = ReadNameOrOrdinal(data, ref offset);
        string title = ReadSz(data, ref offset);
        string font = "";
        if ((style & 0x40) != 0) { // DS_SETFONT
            ushort points = BitConverter.ToUInt16(data, offset); offset += 2;
            if (extended) offset += 4; // weight, italic, charset
            font = points + "pt " + ReadSz(data, ref offset);
        }
        lines.Add(string.Format("DIALOG {0}  style=0x{1:X8} ex=0x{2:X8} {3}x{4} controls={5} font='{6}' title='{7}'",
            label, style, exStyle, cx, cy, count, font, title));
        for (int control = 0; control < count; control++) {
            Align(ref offset, 4);
            uint controlStyle, controlEx; int controlId;
            if (extended) {
                offset += 4; // helpID
                controlEx = BitConverter.ToUInt32(data, offset); offset += 4;
                controlStyle = BitConverter.ToUInt32(data, offset); offset += 4;
            } else {
                controlStyle = BitConverter.ToUInt32(data, offset); offset += 4;
                controlEx = BitConverter.ToUInt32(data, offset); offset += 4;
            }
            short cx0 = BitConverter.ToInt16(data, offset), cy0 = BitConverter.ToInt16(data, offset + 2);
            short cw = BitConverter.ToInt16(data, offset + 4), ch = BitConverter.ToInt16(data, offset + 6); offset += 8;
            if (extended) { controlId = BitConverter.ToInt32(data, offset); offset += 4; }
            else { controlId = BitConverter.ToUInt16(data, offset); offset += 2; }
            ushort classFirst = BitConverter.ToUInt16(data, offset);
            string className;
            if (classFirst == 0xFFFF) {
                ushort atom = BitConverter.ToUInt16(data, offset + 2); offset += 4;
                className = atom >= 0x80 && atom <= 0x85 ? SystemClasses[atom - 0x80] : "atom#" + atom;
            } else className = ReadSz(data, ref offset);
            string text = ReadNameOrOrdinal(data, ref offset);
            ushort extra = BitConverter.ToUInt16(data, offset); offset += 2 + extra;
            lines.Add(string.Format("  {0,-22} id={1,-6} style=0x{2:X8} ex=0x{3:X8} at {4},{5} {6}x{7} '{8}'",
                className, controlId, controlStyle, controlEx, cx0, cy0, cw, ch, text));
        }
    }
}
'@

[DialogResources]::Dump($Path, $Id)
