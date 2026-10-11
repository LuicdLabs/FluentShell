# Dumps a type library (interfaces, their kind, GUID, dual flag, and each method's dispatch id,
# vtable slot, invocation kind and parameter types). Diagnostic only: it loads the library file
# without registering it.
#
#   .\tools\Dump-TypeLib.ps1 C:\Windows\System32\sysmon.ocx
param(
    [Parameter(Mandatory = $true)][string] $Path,
    # Only interfaces whose name contains this text.
    [string] $Filter = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using CT = System.Runtime.InteropServices.ComTypes;

public static class TypeLibDump {
    [DllImport("oleaut32.dll", CharSet = CharSet.Unicode)]
    static extern int LoadTypeLibEx(string file, int regKind, out CT.ITypeLib library);

    static string VarType(CT.TYPEDESC desc, CT.ITypeInfo info) {
        var vt = (VarEnum)desc.vt;
        if (vt == VarEnum.VT_PTR) {
            var inner = (CT.TYPEDESC)Marshal.PtrToStructure(desc.lpValue, typeof(CT.TYPEDESC));
            return VarType(inner, info) + "*";
        }
        if (vt == VarEnum.VT_USERDEFINED) {
            try {
                CT.ITypeInfo target;
                info.GetRefTypeInfo(unchecked((int)(long)desc.lpValue), out target);
                string name, doc, help; int ctx;
                target.GetDocumentation(-1, out name, out doc, out ctx, out help);
                return name;
            } catch { return "userdefined"; }
        }
        if (vt == VarEnum.VT_SAFEARRAY) return "SAFEARRAY";
        return vt.ToString().Replace("VT_", "");
    }

    public static string[] Dump(string file, string filter) {
        var lines = new List<string>();
        CT.ITypeLib library;
        int hr = LoadTypeLibEx(file, 2, out library);   // REGKIND_NONE
        if (hr != 0) { lines.Add("LoadTypeLibEx failed 0x" + hr.ToString("X8")); return lines.ToArray(); }
        int count = library.GetTypeInfoCount();
        for (int i = 0; i < count; ++i) {
            CT.ITypeInfo info; library.GetTypeInfo(i, out info);
            IntPtr attributePointer; info.GetTypeAttr(out attributePointer);
            var attribute = (CT.TYPEATTR)Marshal.PtrToStructure(attributePointer, typeof(CT.TYPEATTR));
            string name, doc, help; int ctx;
            info.GetDocumentation(-1, out name, out doc, out ctx, out help);
            if (filter.Length == 0 || name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) >= 0) {
                lines.Add(string.Format("{0} {1} {2:B} flags=0x{3:X} funcs={4} vars={5}{6}", attribute.typekind, name, attribute.guid,
                    (int)attribute.wTypeFlags, attribute.cFuncs, attribute.cVars,
                    (attribute.wTypeFlags & CT.TYPEFLAGS.TYPEFLAG_FDUAL) != 0 ? " DUAL" : ""));
                // Constants of an enumeration: name = value.
                for (int v = 0; v < attribute.cVars; ++v) {
                    IntPtr variablePointer; info.GetVarDesc(v, out variablePointer);
                    var variable = (CT.VARDESC)Marshal.PtrToStructure(variablePointer, typeof(CT.VARDESC));
                    var variableNames = new string[1]; int variableGot;
                    info.GetNames(variable.memid, variableNames, 1, out variableGot);
                    object constant = null;
                    if (variable.varkind == CT.VARKIND.VAR_CONST && variable.desc.lpvarValue != IntPtr.Zero)
                        constant = Marshal.GetObjectForNativeVariant(variable.desc.lpvarValue);
                    lines.Add(string.Format("    {0} = {1}", variableGot > 0 ? variableNames[0] : "?", constant));
                    info.ReleaseVarDesc(variablePointer);
                }
                for (int j = 0; j < attribute.cFuncs; ++j) {
                    IntPtr functionPointer; info.GetFuncDesc(j, out functionPointer);
                    var function = (CT.FUNCDESC)Marshal.PtrToStructure(functionPointer, typeof(CT.FUNCDESC));
                    var names = new string[function.cParams + 1];
                    int got; info.GetNames(function.memid, names, names.Length, out got);
                    var parameters = new List<string>();
                    long element = function.lprgelemdescParam.ToInt64();
                    for (int p = 0; p < function.cParams; ++p) {
                        var desc = (CT.ELEMDESC)Marshal.PtrToStructure(new IntPtr(element + (long)p * Marshal.SizeOf(typeof(CT.ELEMDESC))), typeof(CT.ELEMDESC));
                        var flags = desc.desc.paramdesc.wParamFlags;
                        string direction = ((flags & CT.PARAMFLAG.PARAMFLAG_FOUT) != 0 ? "out " : "") + ((flags & CT.PARAMFLAG.PARAMFLAG_FIN) != 0 ? "in " : "");
                        parameters.Add(direction + VarType(desc.tdesc, info) + " " + (p + 1 < got ? names[p + 1] : "?"));
                    }
                    lines.Add(string.Format("    id={0,-6} slot={1,-3} {2,-18} {3}({4}) -> {5}", function.memid, function.oVft / IntPtr.Size,
                        function.invkind, got > 0 ? names[0] : "?", string.Join(", ", parameters), VarType(function.elemdescFunc.tdesc, info)));
                    info.ReleaseFuncDesc(functionPointer);
                }
            }
            info.ReleaseTypeAttr(attributePointer);
        }
        return lines.ToArray();
    }
}
'@
[TypeLibDump]::Dump($Path, $Filter)
