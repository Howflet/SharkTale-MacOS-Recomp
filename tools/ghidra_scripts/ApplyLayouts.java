// Create /SharkTale-free root structures named after each class from symbols/layouts.json
// (tools/export_layouts.py), so __thiscall methods decompile with this->Field names.
// Ghidra binds a class namespace to the root-category structure of the same name.
// Usage (headless, saving): -postScript ApplyLayouts.java <layouts.json>
// @category SharkTale
import java.io.File;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;

public class ApplyLayouts extends GhidraScript {
	private DataTypeManager dtm;
	private final Map<String, StructureDataType> pending = new HashMap<>();

	@Override
	protected void run() throws Exception {
		JsonArray all = JsonParser.parseString(
			new String(Files.readAllBytes(new File(getScriptArgs()[0]).toPath()))).getAsJsonArray();
		dtm = currentProgram.getDataTypeManager();
		int made = 0;
		int tx = currentProgram.startTransaction("Apply Shark Tale layouts");
		try {
			// Structs first (classes embed them), then classes.
			for (int pass = 0; pass < 2; pass++) {
				for (JsonElement e : all) {
					JsonObject o = e.getAsJsonObject();
					String name = o.get("name").getAsString();
					boolean isStruct = name.startsWith("F");
					if ((pass == 0) != isStruct) {
						continue;
					}
					Structure s = build(o);
					dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER);
					made++;
				}
			}
		}
		finally {
			currentProgram.endTransaction(tx, true);
		}
		println("Applied " + made + " layouts to " + currentProgram.getName());
	}

	private Structure build(JsonObject o) {
		String name = o.get("name").getAsString();
		int size = o.get("size").getAsInt();
		StructureDataType s = new StructureDataType(CategoryPath.ROOT, name, size, dtm);
		s.setPackingEnabled(false);
		for (JsonElement fe : o.getAsJsonArray("fields")) {
			JsonObject f = fe.getAsJsonObject();
			int off = f.get("offset").getAsInt();
			int len = f.get("size").getAsInt();
			if (off < 0 || len <= 0 || off + len > size) {
				continue;
			}
			DataType t = typeFor(f.get("type").getAsString(), len);
			try {
				s.replaceAtOffset(off, t, t.getLength() > 0 ? t.getLength() : len, f.get("name").getAsString(), null);
			}
			catch (Exception ex) {
				// Overlapping or odd-sized field: leave the bytes undefined.
			}
		}
		return s;
	}

	private DataType typeFor(String type, int len) {
		switch (type) {
			case "int":
				return len == 4 ? IntegerDataType.dataType : new ArrayDataType(IntegerDataType.dataType, len / 4, 4);
			case "float":
				return len == 4 ? FloatDataType.dataType : new ArrayDataType(FloatDataType.dataType, len / 4, 4);
			case "byte":
				return len == 1 ? ByteDataType.dataType : new ArrayDataType(ByteDataType.dataType, len, 1);
			case "ptr":
				return len == 4 ? PointerDataType.dataType : new ArrayDataType(PointerDataType.dataType, len / 4, 4);
			case "name":
				return len == 4 ? DWordDataType.dataType : new ArrayDataType(DWordDataType.dataType, len / 4, 4);
			case "bool":
				return DWordDataType.dataType;
			default:
				if (type.startsWith("struct:")) {
					DataType st = dtm.getDataType(CategoryPath.ROOT, type.substring(7));
					if (st != null && st.getLength() > 0) {
						return len == st.getLength() ? st : new ArrayDataType(st, len / st.getLength(), st.getLength());
					}
				}
				return new ArrayDataType(ByteDataType.dataType, len, 1);
		}
	}
}
