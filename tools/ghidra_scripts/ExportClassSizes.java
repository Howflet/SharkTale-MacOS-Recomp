// Recover sizeof(native class) from IMPLEMENT_CLASS registration calls.
// Every native class constructs its PrivateStaticClass with
//   UClass::UClass(ENativeConstructor, DWORD Size, DWORD ClassFlags, UClass* Base,
//                  UClass* Within, FGuid, const TCHAR* Name, const TCHAR* Package, ...)
// (__thiscall, args pushed right to left). We walk back from each call site and
// read the pushed immediates. MSVC copies the FGuid onto the stack with MOVs, so
// pushes read back as: enum, Size, ClassFlags, Base, Within, Name, Package, ...
// Writes <outdir>/<program>.classsizes.tsv.
// Usage (headless): -postScript ExportClassSizes.java <outdir>
// @category SharkTale
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class ExportClassSizes extends GhidraScript {
	private static final String CTOR_PREFIX = "??0UClass@@QAE@W4ENativeConstructor@@";
	// UClass(EStaticConstructor, Size, ClassFlags, FGuid, Name, Package, ...): no Base/Within.
	private static final String STATIC_CTOR_PREFIX = "??0UClass@@QAE@W4EStaticConstructor@@";

	@Override
	protected void run() throws Exception {
		String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
		String base = currentProgram.getName().replaceAll("\\.[^.]+$", "");
		File out = new File(outDir, base + ".classsizes.tsv");
		int rows = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("callsite\tname\tsize\tclass_flags\tpackage\tbase");
			for (Instruction insn : currentProgram.getListing().getInstructions(true)) {
				if (!insn.getFlowType().isCall()) {
					continue;
				}
				int nameIdx;
				if (callsCtor(insn, CTOR_PREFIX)) {
					nameIdx = 5;
				} else if (callsCtor(insn, STATIC_CTOR_PREFIX)) {
					nameIdx = 3;
				} else {
					continue;
				}
				List<Long> pushes = collectPushes(insn.getAddress());
				if (pushes.size() < nameIdx + 2) {
					w.printf("%s\t?\t?\t?\t?%n", insn.getAddress());
					continue;
				}
				String baseName = nameIdx == 5 ? describe(pushes.get(3), pushSources.get(3)) : "";
				w.printf("%s\t%s\t%d\t%#x\t%s\t%s%n", insn.getAddress(), readWide(pushes.get(nameIdx)),
					pushes.get(1), pushes.get(2), readWide(pushes.get(nameIdx + 1)), baseName);
				rows++;
			}
		}
		println("Wrote " + rows + " class sizes to " + out);
	}

	/** Direct calls land on the export; imported calls go through a labeled IAT slot. */
	private boolean callsCtor(Instruction insn, String prefix) {
		for (Reference ref : insn.getReferencesFrom()) {
			if (isCtor(ref.getToAddress(), prefix)) {
				return true;
			}
			if (ref.getReferenceType().isData()) {
				for (Reference ptr : getReferencesFrom(ref.getToAddress())) {
					if (isCtor(ptr.getToAddress(), prefix)) {
						return true;
					}
				}
			}
		}
		return false;
	}

	/** The demangler makes the readable name primary, so check every label. */
	private boolean isCtor(Address a, String prefix) {
		for (Symbol s : currentProgram.getSymbolTable().getSymbols(a)) {
			if (s.getName().startsWith(prefix)) {
				return true;
			}
		}
		if (a.isExternalAddress()) {
			ghidra.program.model.symbol.ExternalLocation loc =
				currentProgram.getExternalManager().getExternalLocation(getSymbolAt(a));
			return loc != null && loc.getOriginalImportedName() != null
				&& loc.getOriginalImportedName().startsWith(prefix);
		}
		return false;
	}

	/** For each push in the last collectPushes: the call that produced a pushed register. */
	private List<Instruction> pushSources = new ArrayList<>();

	/** Name the class a pushed UClass* refers to (PrivateStaticClass address or StaticClass() call). */
	private String describe(long value, Instruction producer) {
		Address target = null;
		if (value > 0) {
			target = toAddr(value);
		} else if (producer != null) {
			for (Reference r : producer.getReferencesFrom()) {
				target = r.getToAddress();
				if (r.getReferenceType().isData()) {
					for (Reference ptr : getReferencesFrom(target)) {
						target = ptr.getToAddress();
					}
				}
			}
		}
		if (target == null) {
			return "?";
		}
		Symbol best = null;
		for (Symbol s : currentProgram.getSymbolTable().getSymbols(target)) {
			if (s.getName().startsWith("?")) {
				best = s;
			}
		}
		if (best == null && target.isExternalAddress()) {
			ghidra.program.model.symbol.ExternalLocation loc =
				currentProgram.getExternalManager().getExternalLocation(getSymbolAt(target));
			return loc != null && loc.getOriginalImportedName() != null ? loc.getOriginalImportedName() : "?";
		}
		return best != null ? best.getName() : "?";
	}

	/** PUSH operands walking backwards from the call; register pushes read as -1. */
	private List<Long> collectPushes(Address call) {
		List<Long> pushes = new ArrayList<>();
		pushSources = new ArrayList<>();
		Instruction insn = getInstructionAt(call);
		for (int i = 0; i < 80 && insn != null && pushes.size() < 12; i++) {
			insn = insn.getPrevious();
			if (insn == null || insn.getFlowType().isTerminal()) {
				break;
			}
			if (!insn.getMnemonicString().equalsIgnoreCase("PUSH")) {
				continue;
			}
			Object[] ops = insn.getOpObjects(0);
			pushSources.add(producerOf(insn));
			if (ops.length == 1 && ops[0] instanceof Scalar) {
				pushes.add(((Scalar) ops[0]).getUnsignedValue());
			} else if (ops.length == 1 && ops[0] instanceof Address) {
				pushes.add(((Address) ops[0]).getOffset());
			} else {
				pushes.add(-1L);
			}
		}
		return pushes;
	}

	/** The CALL immediately before a register PUSH (e.g. call Base::StaticClass; push eax). */
	private Instruction producerOf(Instruction push) {
		Instruction prev = push.getPrevious();
		for (int i = 0; i < 3 && prev != null; i++, prev = prev.getPrevious()) {
			if (prev.getFlowType().isCall()) {
				return prev;
			}
		}
		return null;
	}

	private String readWide(long offset) {
		if (offset <= 0) {
			return "?";
		}
		try {
			Address a = toAddr(offset);
			StringBuilder sb = new StringBuilder();
			for (int i = 0; i < 128; i++) {
				char c = (char) (getShort(a.add(i * 2)) & 0xffff);
				if (c == 0) {
					break;
				}
				sb.append(c);
			}
			return sb.length() > 0 ? sb.toString() : "?";
		} catch (Exception e) {
			return "?";
		}
	}
}
