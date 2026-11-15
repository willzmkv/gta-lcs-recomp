// Decompile all functions in the current program and export to a C file
//@category Decompiler
//@keybinding
//@menupath
//@toolbar

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = (args.length > 0) ? args[0] : "build/ghidra/export/decompiled.c";
        
        File outFile = new File(outPath);
        outFile.getParentFile().mkdirs();

        println("Exporting decompilation to: " + outFile.getAbsolutePath());

        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);

        PrintWriter writer = new PrintWriter(outFile, "UTF-8");
        writer.println("// Decompiled with Ghidra for " + currentProgram.getName());
        writer.println();

        FunctionIterator iter = currentProgram.getFunctionManager().getFunctions(true);
        List<Function> funcs = new ArrayList<>();
        while (iter.hasNext()) {
            funcs.add(iter.next());
        }

        Collections.sort(funcs, new Comparator<Function>() {
            @Override
            public int compare(Function a, Function b) {
                return a.getEntryPoint().compareTo(b.getEntryPoint());
            }
        });

        int total = funcs.size();
        int count = 0;
        for (Function f : funcs) {
            count++;
            if (f.isThunk() || f.isExternal()) {
                continue;
            }
            DecompileResults res = ifc.decompileFunction(f, 30, monitor);
            if (res != null && res.decompileCompleted()) {
                writer.println("// Address: " + f.getEntryPoint());
                writer.println(res.getDecompiledFunction().getC());
                writer.println();
            }
            if (count % 100 == 0) {
                println("Decompiled " + count + " / " + total + " functions...");
            }
        }

        writer.close();
        ifc.dispose();
        println("Decompilation export completed! Total functions: " + count);
    }
}
