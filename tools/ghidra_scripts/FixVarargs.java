// Give UE2's variadic logging/formatting functions correct prototypes so the
// decompiler shows their format strings. These are __cdecl members (this is
// the first stack argument) or free functions.
// Usage (headless, saving): -postScript FixVarargs.java
// @category SharkTale
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.symbol.SourceType;

public class FixVarargs extends GhidraScript {
	private static final DataType TCHAR_PTR = new PointerDataType(UnsignedShortDataType.dataType);
	private static final DataType CHAR_PTR = new PointerDataType(CharDataType.dataType);
	private static final DataType PTR = PointerDataType.dataType;
	private static final DataType INT = IntegerDataType.dataType;

	@Override
	protected void run() throws Exception {
		int fixed = 0;
		List<Function> all = new ArrayList<>();
		FunctionIterator ext = currentProgram.getFunctionManager().getExternalFunctions();
		while (ext.hasNext()) {
			all.add(ext.next());
		}
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			all.add(f);
		}
		for (Function f : all) {
			String q = f.getName(true);
			String mangled = f.getSymbol().getName();
			DataType[] params = null;
			String[] names = null;
			if (q.endsWith("FOutputDevice::Logf") || q.endsWith("FOutputDevice::Logf_")) {
				// ?Logf@FOutputDevice@@QAAXW4EName@@PBGZZ vs ?Logf@FOutputDevice@@QAAXPBGZZ
				boolean withEvent = f.getParameterCount() != 2 && !isFormatOnly(f);
				params = withEvent ? new DataType[] { PTR, INT, TCHAR_PTR } : new DataType[] { PTR, TCHAR_PTR };
				names = withEvent ? new String[] { "This", "Event", "Fmt" } : new String[] { "This", "Fmt" };
			}
			else if (q.endsWith("appErrorf") || q.endsWith("appThrowf") || q.endsWith("appUnwindf")
					|| q.endsWith("appDebugMessagef") || q.endsWith("appMsgf")) {
				params = new DataType[] { TCHAR_PTR };
				names = new String[] { "Fmt" };
			}
			else if (q.endsWith("appSprintf")) {
				params = new DataType[] { TCHAR_PTR, TCHAR_PTR };
				names = new String[] { "Dest", "Fmt" };
			}
			else if (q.endsWith("FString::Printf")) {
				params = new DataType[] { PTR, TCHAR_PTR };
				names = new String[] { "Result", "Fmt" };
			}
			if (params == null) {
				continue;
			}
			List<ParameterImpl> ps = new ArrayList<>();
			for (int i = 0; i < params.length; i++) {
				ps.add(new ParameterImpl(names[i], params[i], currentProgram));
			}
			f.updateFunction("__cdecl", null, ps, FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true,
				SourceType.USER_DEFINED);
			f.setVarArgs(true);
			fixed++;
		}
		println("Fixed " + fixed + " variadic prototypes in " + currentProgram.getName());
	}

	/** Logf(const TCHAR* Fmt, ...) overload: mangled with PBGZZ right after the class. */
	private boolean isFormatOnly(Function f) {
		for (ghidra.program.model.symbol.Symbol s : currentProgram.getSymbolTable().getSymbols(f.getEntryPoint())) {
			if (s.getName().contains("@@QAAXPBGZZ")) {
				return true;
			}
		}
		if (f.isExternal() && f.getExternalLocation() != null) {
			String orig = f.getExternalLocation().getOriginalImportedName();
			return orig != null && orig.contains("@@QAAXPBGZZ");
		}
		return false;
	}
}
