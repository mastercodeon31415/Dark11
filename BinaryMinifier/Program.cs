using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace BinaryHyperMinifier
{
    class Program
    {
        // Global maps to ensure consistency across the whole file
        static Dictionary<string, string> _globalTokenMap = new Dictionary<string, string>();
        static int _funcCounter = 0;
        static int _dataCounter = 0;
        static int _varCounter = 0;
        static int _localCounter = 0;

        static void Main(string[] args)
        {
            string inputFile = "";
            if (args.Length >= 1) inputFile = args[0];
            else
            {
                Console.WriteLine("Drag and drop your .c file here:");
                inputFile = Console.ReadLine().Trim('"');
            }

            if (!File.Exists(inputFile)) return;

            string outputFile = Path.ChangeExtension(inputFile, ".hyper.c");
            string sourceCode = File.ReadAllText(inputFile);
            long originalLength = sourceCode.Length;

            Console.WriteLine("Applying Hyper Optimization...");

            // 1. Basic Cleaning
            string code = MinifyBasic(sourceCode);

            // 2. Shorten Types (undefined8 -> u8)
            code = ShortenTypes(code);

            // 3. Strip Casts
            code = StripCasts(code);

            // 4. Rename Functions (FUN_...) & Data (DAT_...)
            code = TokenizeGlobals(code);

            // 5. [NEW] Rename Parameters (param_1 -> p1)
            code = TokenizeParams(code);

            // 6. [NEW] Rename Locals (local_res8 -> l1, iVar1 -> v1)
            code = TokenizeLocals(code);

            // 7. [NEW] Micro-optimizations (0x0 -> 0)
            code = MicroOptimize(code);

            // 8. Add Legend
            string header = "// LEGEND: f=func, d=data, p=param, l=local, v=var, u8=undefined8, i64=longlong\n";
            code = header + code;

            File.WriteAllText(outputFile, code);

            long newLength = code.Length;
            double savings = 100.0 * (1.0 - ((double)newLength / originalLength));

            Console.WriteLine($"Original: {originalLength} chars");
            Console.WriteLine($"Final:    {newLength} chars");
            Console.WriteLine($"Reduced:  {savings:F2}%");
            Console.WriteLine($"Saved to: {outputFile}");

            if (args.Length == 0) Console.ReadKey();
        }

        static string MinifyBasic(string input)
        {
            input = Regex.Replace(input, @"/\*.*?\*/", "", RegexOptions.Singleline);
            input = Regex.Replace(input, @"//.*", "");
            input = Regex.Replace(input, @"\s+", " ");
            input = Regex.Replace(input, @"\s+([;{}(),=><!+\-*/%&|\[\]^])", "$1");
            input = Regex.Replace(input, @"([;{}(),=><!+\-*/%&|\[\]^])\s+", "$1");
            return input.Trim();
        }

        static string ShortenTypes(string input)
        {
            var replacements = new Dictionary<string, string>
            {
                { "undefined8", "u8" }, { "undefined4", "u4" }, { "undefined2", "u2" },
                { "undefined1", "u1" }, { "undefined",  "u" },  { "longlong",   "i64" },
                { "ulonglong",  "u64" }, { "uint",       "u32" },{ "ushort",     "u16" },
                { "byte",       "u8" }, { "void",       "v" },  { "LPCITEMIDLIST", "itm" },
                { "LPITEMIDLIST", "itm" },{ "LPCWSTR",    "ws" }, { "LPWSTR",     "ws" },
                { "LPSTR",      "s" },  { "operator_new", "new"}, { "operator_delete", "del"}
            };
            foreach (var kvp in replacements)
                input = Regex.Replace(input, $@"\b{kvp.Key}\b", kvp.Value);
            return input;
        }

        static string StripCasts(string input)
        {
            // Remove (type *) and (type) casts
            string[] castTypes = { "u8", "u4", "u2", "u1", "i64", "u64", "int", "char", "short", "float", "double", "bool", "v", "ws", "s", "itm" };
            string typePattern = string.Join("|", castTypes);
            string pattern = $@"\((?:{typePattern})[\s\*]*\)";
            return Regex.Replace(input, pattern, "");
        }

        static string TokenizeGlobals(string input)
        {
            // Rename FUN_... -> f1, DAT_... -> d1
            input = Regex.Replace(input, @"\bFUN_[0-9a-fA-F]+\b", m => GetToken(m.Value, "f", ref _funcCounter));
            input = Regex.Replace(input, @"\bDAT_[0-9a-fA-F]+\b", m => GetToken(m.Value, "d", ref _dataCounter));
            input = Regex.Replace(input, @"\b_DAT_[0-9a-fA-F]+\b", m => GetToken(m.Value, "d", ref _dataCounter));
            input = Regex.Replace(input, @"\bPTR_DAT_[0-9a-fA-F]+\b", m => GetToken(m.Value, "pd", ref _dataCounter));
            return input;
        }

        static string TokenizeParams(string input)
        {
            // param_1 -> p1, param_2 -> p2
            // We use Regex to catch "param_" followed by numbers
            return Regex.Replace(input, @"\bparam_([0-9]+)\b", "p$1");
        }

        static string TokenizeLocals(string input)
        {
            // 1. Rename Ghidra generated locals (local_res8, local_4f8, uStack_10)
            // We map these to l1, l2, l3... globally to save token variety
            input = Regex.Replace(input, @"\b(local_|uStack_|iStack_)[0-9a-z_]+\b", m => GetToken(m.Value, "l", ref _localCounter));

            // 2. Rename Ghidra typed vars (iVar1, bVar2, pIVar3)
            // Matches [a-z]Var[0-9]
            input = Regex.Replace(input, @"\b[a-zA-Z]+Var[0-9]+\b", m => GetToken(m.Value, "v", ref _varCounter));

            return input;
        }

        static string MicroOptimize(string input)
        {
            // Replace 0x0 with 0 (saves 2 chars, often 1 token difference)
            input = Regex.Replace(input, @"\b0x0\b", "0");

            // Remove 'return;' inside void functions (Ghidra leaves them at end of blocks)
            input = input.Replace("return;}", "}");

            return input;
        }

        static string GetToken(string key, string prefix, ref int counter)
        {
            if (!_globalTokenMap.ContainsKey(key))
            {
                _globalTokenMap[key] = prefix + (++counter);
            }
            return _globalTokenMap[key];
        }
    }
}