// Decompile every function whose qualified name contains one of the given
// substrings, writing the C to <outfile>.
// Usage (headless, -process <module>):
//   -postScript DecompileMatching.java <outfile> <substring> [<substring> ...]
// @category SharkTale
import java.io.File;
import java.io.PrintWriter;
import java.util.Arrays;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileMatching extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args[0]);
		List<String> needles = Arrays.asList(args).subList(1, args.length);
		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		int count = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				String name = f.getName(true);
				if (f.isThunk() || needles.stream().noneMatch(name::contains)) {
					continue;
				}
				DecompileResults r = ifc.decompileFunction(f, 60, monitor);
				w.printf("// %s @ %s%n", name, f.getEntryPoint());
				w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC()
					: "// decompile failed: " + r.getErrorMessage());
				count++;
			}
		}
		ifc.dispose();
		println("Decompiled " + count + " functions to " + out);
	}
}
