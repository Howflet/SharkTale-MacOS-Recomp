// Write every function in the current program to <outdir>/<program>.functions.tsv
// (entry, size, is_thunk, namespace-qualified name).
// Usage (headless): -postScript ExportFunctions.java <outdir>
// @category SharkTale
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class ExportFunctions extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
		String base = currentProgram.getName().replaceAll("\\.[^.]+$", "");
		File out = new File(outDir, base + ".functions.tsv");
		int count = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("entry\tsize\tthunk\tname");
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				w.printf("%s\t%d\t%d\t%s%n", f.getEntryPoint(), f.getBody().getNumAddresses(),
					f.isThunk() ? 1 : 0, f.getName(true));
				count++;
			}
		}
		println("Exported " + count + " functions to " + out);
	}
}
