using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.IO;

namespace DarkLoader
{
    class Program
    {
        const uint PROCESS_CREATE_THREAD = 0x0002;
        const uint PROCESS_QUERY_INFORMATION = 0x0400;
        const uint PROCESS_VM_OPERATION = 0x0008;
        const uint PROCESS_VM_WRITE = 0x0020;
        const uint PROCESS_VM_READ = 0x0010;
        const uint MEM_COMMIT = 0x1000;
        const uint PAGE_READWRITE = 0x04;

        [DllImport("kernel32.dll")]
        static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);

        [DllImport("kernel32.dll", CharSet = CharSet.Auto)]
        public static extern IntPtr GetModuleHandle(string lpModuleName);

        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
        static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

        [DllImport("kernel32.dll")]
        static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);

        [DllImport("kernel32.dll")]
        static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out IntPtr lpNumberOfBytesWritten);

        [DllImport("kernel32.dll")]
        static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, IntPtr lpThreadId);

        [DllImport("kernel32.dll")]
        static extern int WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll")]
        static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress, int dwSize, int dwFreeType);

        static void Main(string[] args)
        {
            Console.Title = "Dark Mode Injector";
            string dllPath = Path.GetFullPath("DarkEnforcer.dll");

            if (!File.Exists(dllPath))
            {
                Console.WriteLine("Error: DarkEnforcer.dll not found in " + dllPath);
                return;
            }

            Console.WriteLine("Targeting Explorer...");
            Process[] explorers = Process.GetProcessesByName("explorer");
            if (explorers.Length == 0) return;

            Process target = explorers[0];
            Console.WriteLine($"Found Explorer PID: {target.Id}");

            // 1. INJECTION
            IntPtr hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, false, target.Id);

            IntPtr pRemotePath = VirtualAllocEx(hProcess, IntPtr.Zero, (uint)dllPath.Length + 1, MEM_COMMIT, PAGE_READWRITE);
            byte[] bytes = Encoding.Default.GetBytes(dllPath);
            WriteProcessMemory(hProcess, pRemotePath, bytes, (uint)bytes.Length, out _);

            IntPtr pLoadLibrary = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryA");
            IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0, pLoadLibrary, pRemotePath, 0, IntPtr.Zero);

            WaitForSingleObject(hThread, 5000);
            VirtualFreeEx(hProcess, pRemotePath, 0, 0x8000);
            CloseHandle(hThread);
            CloseHandle(hProcess);

            Console.WriteLine("Payload Injected. OPEN THE RUN BOX NOW (Win+R).");
            Console.WriteLine("\n[!] PRESS ENTER TO EJECT DLL AND UNLOCK FILE [!]\n");
            Console.ReadLine();

            // 2. EJECTION
            Console.WriteLine("Ejecting...");
            EjectDll(target.Id, "DarkEnforcer.dll");
        }

        static void EjectDll(int processId, string dllName)
        {
            Process p = Process.GetProcessById(processId);
            // Refresh module list
            p.Refresh();
            foreach (ProcessModule mod in p.Modules)
            {
                if (mod.ModuleName.Equals(dllName, StringComparison.OrdinalIgnoreCase))
                {
                    IntPtr hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, false, processId);
                    IntPtr pFreeLibrary = GetProcAddress(GetModuleHandle("kernel32.dll"), "FreeLibrary");
                    IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0, pFreeLibrary, mod.BaseAddress, 0, IntPtr.Zero);
                    WaitForSingleObject(hThread, 5000);
                    CloseHandle(hThread);
                    CloseHandle(hProcess);
                    Console.WriteLine("DLL Ejected. You may now recompile.");
                    return;
                }
            }
            Console.WriteLine("DLL not found in target process (already ejected?)");
        }
    }
}