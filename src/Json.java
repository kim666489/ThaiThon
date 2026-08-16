import java.io.IOException;
import java.io.Reader;
import java.io.StringReader;
import java.io.UncheckedIOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Json — single-file JSON library สำหรับ Java (ไม่มี dependency ภายนอก)
 * ------------------------------------------------------------------
 * ความสามารถ:
 *   - Parse ข้อความ JSON (String หรือ Reader) เป็นโครงสร้างต้นไม้ {@link Json.Node}
 *   - รองรับ object, array, string (พร้อม escape/\\uXXXX), number (int/double/exponent),
 *     true, false, null ตามสเปก JSON มาตรฐาน (RFC 8259)
 *   - Serialize กลับเป็น String ได้ทั้งแบบ compact และ pretty-print
 *   - สร้างโครงสร้าง JSON เองผ่าน builder API แบบ fluent (method chaining)
 *   - เข้าถึงข้อมูลแบบ nested ผ่าน path string เช่น "user.address[0].city"
 *   - แปลงชนิดข้อมูลอัตโนมัติแบบปลอดภัย (as* / opt* methods ไม่ throw)
 *
 * ตัวอย่างการใช้งานพื้นฐาน:
 *
 *   Json.Node root = Json.parse("{\"name\":\"Tom\",\"age\":30,\"tags\":[\"a\",\"b\"]}");
 *   String name = root.get("name").asString();       // "Tom"
 *   int age = root.get("age").asInt();                // 30
 *   String tag0 = root.path("tags[0]").asString();     // "a"
 *
 *   Json.Node obj = Json.object()
 *       .put("id", 1)
 *       .put("active", true)
 *       .put("meta", Json.object().put("k", "v"));
 *   System.out.println(obj.toPrettyString());
 *
 * ไม่มี main() ในไฟล์นี้ — เป็นไฟล์ library ล้วน ๆ ใช้ import แล้วเรียก Json.* ได้ทันที
 */
public final class Json {

    private Json() {
    }

    // ==================================================================
    //  Public static entry points
    // ==================================================================

    /** parse ข้อความ JSON เป็น {@link Node}. โยน {@link JsonParseException} ถ้ารูปแบบไม่ถูกต้อง */
    public static Node parse(String jsonText) {
        if (jsonText == null) {
            throw new JsonParseException("Input JSON string is null", 0);
        }
        Parser parser = new Parser(jsonText);
        Node result = parser.parseRoot();
        return result;
    }

    /** parse จาก Reader (เช่นอ่านจากไฟล์หรือ network stream) */
    public static Node parse(Reader reader) throws IOException {
        StringBuilder sb = new StringBuilder();
        char[] buf = new char[4096];
        int n;
        while ((n = reader.read(buf)) != -1) {
            sb.append(buf, 0, n);
        }
        return parse(sb.toString());
    }

    /** สร้าง Node ชนิด object ว่าง ๆ สำหรับเริ่ม build เอง */
    public static Node object() {
        return new Node(Type.OBJECT);
    }

    /** สร้าง Node ชนิด array ว่าง ๆ สำหรับเริ่ม build เอง */
    public static Node array() {
        return new Node(Type.ARRAY);
    }

    public static Node of(String value) {
        return value == null ? nullNode() : Node.string(value);
    }

    public static Node of(long value) {
        return Node.number(value, true);
    }

    public static Node of(double value) {
        return Node.number(value, false);
    }

    public static Node of(boolean value) {
        return Node.bool(value);
    }

    public static Node nullNode() {
        return Node.NULL_INSTANCE;
    }

    // ==================================================================
    //  Node — ตัวแทนของค่า JSON ใด ๆ (object / array / string / number / boolean / null)
    // ==================================================================

    public enum Type { OBJECT, ARRAY, STRING, NUMBER, BOOLEAN, NULL }

    public static final class Node implements Iterable<Node> {

        private static final Node NULL_INSTANCE = new Node(Type.NULL);

        private final Type type;
        private LinkedHashMap<String, Node> objectValue;
        private List<Node> arrayValue;
        private String stringValue;
        private double numberValue;
        private boolean numberIsIntegral;
        private boolean booleanValue;

        private Node(Type type) {
            this.type = type;
            if (type == Type.OBJECT) objectValue = new LinkedHashMap<>();
            if (type == Type.ARRAY) arrayValue = new ArrayList<>();
        }

        private static Node string(String v) {
            Node n = new Node(Type.STRING);
            n.stringValue = v;
            return n;
        }

        private static Node number(double v, boolean integral) {
            Node n = new Node(Type.NUMBER);
            n.numberValue = v;
            n.numberIsIntegral = integral;
            return n;
        }

        private static Node bool(boolean v) {
            Node n = new Node(Type.BOOLEAN);
            n.booleanValue = v;
            return n;
        }

        // ---------------- Type checks ----------------

        public Type type() { return type; }
        public boolean isObject() { return type == Type.OBJECT; }
        public boolean isArray() { return type == Type.ARRAY; }
        public boolean isString() { return type == Type.STRING; }
        public boolean isNumber() { return type == Type.NUMBER; }
        public boolean isBoolean() { return type == Type.BOOLEAN; }
        public boolean isNull() { return type == Type.NULL; }

        // ---------------- Object mutation (fluent) ----------------

        private void requireObject() {
            if (type != Type.OBJECT) {
                throw new IllegalStateException("Node is not an OBJECT (actual type: " + type + ")");
            }
        }

        private void requireArray() {
            if (type != Type.ARRAY) {
                throw new IllegalStateException("Node is not an ARRAY (actual type: " + type + ")");
            }
        }

        public Node put(String key, Node value) {
            requireObject();
            objectValue.put(key, value == null ? NULL_INSTANCE : value);
            return this;
        }

        public Node put(String key, String value) { return put(key, of(value)); }
        public Node put(String key, long value) { return put(key, of(value)); }
        public Node put(String key, double value) { return put(key, of(value)); }
        public Node put(String key, boolean value) { return put(key, of(value)); }

        public Node remove(String key) {
            requireObject();
            objectValue.remove(key);
            return this;
        }

        public boolean has(String key) {
            requireObject();
            return objectValue.containsKey(key);
        }

        public Set<String> keys() {
            requireObject();
            return Collections.unmodifiableSet(objectValue.keySet());
        }

        // ---------------- Array mutation (fluent) ----------------

        public Node add(Node value) {
            requireArray();
            arrayValue.add(value == null ? NULL_INSTANCE : value);
            return this;
        }

        public Node add(String value) { return add(of(value)); }
        public Node add(long value) { return add(of(value)); }
        public Node add(double value) { return add(of(value)); }
        public Node add(boolean value) { return add(of(value)); }

        public Node removeAt(int index) {
            requireArray();
            arrayValue.remove(index);
            return this;
        }

        // ---------------- Access ----------------

        /** สำหรับ object: คืนค่าตาม key, ถ้าไม่มี key คืน null (ของ Java ไม่ใช่ Json.Node.nullNode()) */
        public Node get(String key) {
            requireObject();
            return objectValue.get(key);
        }

        /** เหมือน get(key) แต่ถ้าไม่มี key จะคืน Json null node แทนที่จะคืน Java null (ปลอดภัยต่อ NPE เวลา chain) */
        public Node getOrNull(String key) {
            requireObject();
            Node v = objectValue.get(key);
            return v == null ? NULL_INSTANCE : v;
        }

        /** สำหรับ array: คืนค่าตาม index */
        public Node get(int index) {
            requireArray();
            return arrayValue.get(index);
        }

        /** จำนวนสมาชิก (object: จำนวน key, array: ความยาว, อื่น ๆ: 0) */
        public int size() {
            if (type == Type.OBJECT) return objectValue.size();
            if (type == Type.ARRAY) return arrayValue.size();
            return 0;
        }

        public boolean isEmpty() { return size() == 0; }

        public List<Node> values() {
            requireArray();
            return Collections.unmodifiableList(arrayValue);
        }

        public Map<String, Node> entries() {
            requireObject();
            return Collections.unmodifiableMap(objectValue);
        }

        @Override
        public Iterator<Node> iterator() {
            requireArray();
            return values().iterator();
        }

        /**
         * เข้าถึงค่าแบบ nested path เช่น "a.b.c" หรือ "items[2].name" หรือ "list[0][1]"
         * คืน Json null node ถ้า path ไม่พบ (ไม่ throw) เพื่อให้ chain ต่อได้ปลอดภัย
         */
        public Node path(String dotPath) {
            if (dotPath == null || dotPath.isEmpty()) return this;
            Node current = this;
            int i = 0;
            int len = dotPath.length();
            StringBuilder token = new StringBuilder();

            while (i < len) {
                char c = dotPath.charAt(i);
                if (c == '.') {
                    if (token.length() > 0) {
                        current = stepKey(current, token.toString());
                        token.setLength(0);
                    }
                    i++;
                } else if (c == '[') {
                    if (token.length() > 0) {
                        current = stepKey(current, token.toString());
                        token.setLength(0);
                    }
                    int close = dotPath.indexOf(']', i);
                    if (close < 0) {
                        throw new IllegalArgumentException("Malformed path (missing ']'): " + dotPath);
                    }
                    String idxStr = dotPath.substring(i + 1, close);
                    int idx;
                    try {
                        idx = Integer.parseInt(idxStr.trim());
                    } catch (NumberFormatException e) {
                        throw new IllegalArgumentException("Malformed array index in path: " + dotPath);
                    }
                    current = stepIndex(current, idx);
                    i = close + 1;
                } else {
                    token.append(c);
                    i++;
                }
            }
            if (token.length() > 0) {
                current = stepKey(current, token.toString());
            }
            return current == null ? NULL_INSTANCE : current;
        }

        private static Node stepKey(Node node, String key) {
            if (node == null || node.type != Type.OBJECT) return NULL_INSTANCE;
            Node next = node.objectValue.get(key);
            return next == null ? NULL_INSTANCE : next;
        }

        private static Node stepIndex(Node node, int idx) {
            if (node == null || node.type != Type.ARRAY) return NULL_INSTANCE;
            if (idx < 0 || idx >= node.arrayValue.size()) return NULL_INSTANCE;
            return node.arrayValue.get(idx);
        }

        // ---------------- Safe conversions (ไม่ throw, มีค่า default) ----------------

        public String asString() { return asString(null); }

        public String asString(String defaultValue) {
            switch (type) {
                case STRING: return stringValue;
                case NUMBER: return numberIsIntegral ? Long.toString((long) numberValue) : Double.toString(numberValue);
                case BOOLEAN: return Boolean.toString(booleanValue);
                case NULL: return defaultValue;
                default: return defaultValue;
            }
        }

        public int asInt() { return asInt(0); }

        public int asInt(int defaultValue) {
            if (type == Type.NUMBER) return (int) numberValue;
            if (type == Type.STRING) {
                try { return Integer.parseInt(stringValue.trim()); } catch (NumberFormatException e) { return defaultValue; }
            }
            return defaultValue;
        }

        public long asLong() { return asLong(0L); }

        public long asLong(long defaultValue) {
            if (type == Type.NUMBER) return (long) numberValue;
            if (type == Type.STRING) {
                try { return Long.parseLong(stringValue.trim()); } catch (NumberFormatException e) { return defaultValue; }
            }
            return defaultValue;
        }

        public double asDouble() { return asDouble(0.0); }

        public double asDouble(double defaultValue) {
            if (type == Type.NUMBER) return numberValue;
            if (type == Type.STRING) {
                try { return Double.parseDouble(stringValue.trim()); } catch (NumberFormatException e) { return defaultValue; }
            }
            return defaultValue;
        }

        public boolean asBoolean() { return asBoolean(false); }

        public boolean asBoolean(boolean defaultValue) {
            if (type == Type.BOOLEAN) return booleanValue;
            if (type == Type.STRING) return Boolean.parseBoolean(stringValue);
            if (type == Type.NUMBER) return numberValue != 0;
            return defaultValue;
        }

        // ---------------- Serialization ----------------

        /** แปลงเป็น JSON string แบบ compact (ไม่มี whitespace เกิน) */
        @Override
        public String toString() {
            StringBuilder sb = new StringBuilder();
            write(sb, -1, 0);
            return sb.toString();
        }

        /** แปลงเป็น JSON string แบบ pretty-print ย่อหน้าอ่านง่าย ใช้ 2 space ต่อระดับ */
        public String toPrettyString() {
            return toPrettyString(2);
        }

        /** แปลงเป็น JSON string แบบ pretty-print กำหนดจำนวน space ต่อระดับเอง */
        public String toPrettyString(int indentSpaces) {
            StringBuilder sb = new StringBuilder();
            write(sb, indentSpaces, 0);
            return sb.toString();
        }

        private void write(StringBuilder sb, int indent, int depth) {
            switch (type) {
                case NULL:
                    sb.append("null");
                    break;
                case BOOLEAN:
                    sb.append(booleanValue);
                    break;
                case NUMBER:
                    if (numberIsIntegral) {
                        sb.append((long) numberValue);
                    } else {
                        if (Double.isNaN(numberValue) || Double.isInfinite(numberValue)) {
                            sb.append("null"); // JSON ไม่รองรับ NaN/Infinity โดยตรง
                        } else {
                            sb.append(numberValue);
                        }
                    }
                    break;
                case STRING:
                    writeEscapedString(sb, stringValue);
                    break;
                case ARRAY:
                    writeArray(sb, indent, depth);
                    break;
                case OBJECT:
                    writeObject(sb, indent, depth);
                    break;
            }
        }

        private void writeArray(StringBuilder sb, int indent, int depth) {
            if (arrayValue.isEmpty()) {
                sb.append("[]");
                return;
            }
            sb.append('[');
            boolean pretty = indent >= 0;
            for (int i = 0; i < arrayValue.size(); i++) {
                if (pretty) newlineIndent(sb, indent, depth + 1);
                arrayValue.get(i).write(sb, indent, depth + 1);
                if (i < arrayValue.size() - 1) sb.append(',');
                if (!pretty && i < arrayValue.size() - 1) sb.append(' ');
            }
            if (pretty) newlineIndent(sb, indent, depth);
            sb.append(']');
        }

        private void writeObject(StringBuilder sb, int indent, int depth) {
            if (objectValue.isEmpty()) {
                sb.append("{}");
                return;
            }
            sb.append('{');
            boolean pretty = indent >= 0;
            int i = 0;
            int last = objectValue.size() - 1;
            for (Map.Entry<String, Node> e : objectValue.entrySet()) {
                if (pretty) newlineIndent(sb, indent, depth + 1);
                writeEscapedString(sb, e.getKey());
                sb.append(':');
                if (pretty) sb.append(' ');
                e.getValue().write(sb, indent, depth + 1);
                if (i < last) sb.append(',');
                if (!pretty && i < last) sb.append(' ');
                i++;
            }
            if (pretty) newlineIndent(sb, indent, depth);
            sb.append('}');
        }

        private static void newlineIndent(StringBuilder sb, int indent, int depth) {
            sb.append('\n');
            for (int i = 0; i < indent * depth; i++) sb.append(' ');
        }

        private static void writeEscapedString(StringBuilder sb, String s) {
            sb.append('"');
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                switch (c) {
                    case '"': sb.append("\\\""); break;
                    case '\\': sb.append("\\\\"); break;
                    case '\b': sb.append("\\b"); break;
                    case '\f': sb.append("\\f"); break;
                    case '\n': sb.append("\\n"); break;
                    case '\r': sb.append("\\r"); break;
                    case '\t': sb.append("\\t"); break;
                    default:
                        if (c < 0x20) {
                            sb.append(String.format("\\u%04x", (int) c));
                        } else {
                            sb.append(c);
                        }
                }
            }
            sb.append('"');
        }

        /** deep copy ของ Node (ใช้เวลาต้องการแก้ไขโดยไม่กระทบต้นฉบับ) */
        public Node deepCopy() {
            switch (type) {
                case OBJECT: {
                    Node copy = new Node(Type.OBJECT);
                    for (Map.Entry<String, Node> e : objectValue.entrySet()) {
                        copy.objectValue.put(e.getKey(), e.getValue().deepCopy());
                    }
                    return copy;
                }
                case ARRAY: {
                    Node copy = new Node(Type.ARRAY);
                    for (Node v : arrayValue) copy.arrayValue.add(v.deepCopy());
                    return copy;
                }
                case STRING: return Node.string(stringValue);
                case NUMBER: return Node.number(numberValue, numberIsIntegral);
                case BOOLEAN: return Node.bool(booleanValue);
                default: return NULL_INSTANCE;
            }
        }
    }

    // ==================================================================
    //  JsonParseException
    // ==================================================================

    public static final class JsonParseException extends RuntimeException {
        private final int position;

        public JsonParseException(String message, int position) {
            super(message + " (at position " + position + ")");
            this.position = position;
        }

        public int position() { return position; }
    }

    // ==================================================================
    //  Parser — recursive-descent JSON parser
    // ==================================================================

    private static final class Parser {
        private final String src;
        private final int len;
        private int pos;

        Parser(String src) {
            this.src = src;
            this.len = src.length();
            this.pos = 0;
        }

        Node parseRoot() {
            skipWhitespace();
            Node value = parseValue();
            skipWhitespace();
            if (pos != len) {
                throw new JsonParseException("Unexpected trailing characters after JSON value", pos);
            }
            return value;
        }

        private Node parseValue() {
            skipWhitespace();
            if (pos >= len) {
                throw new JsonParseException("Unexpected end of input while expecting a value", pos);
            }
            char c = src.charAt(pos);
            switch (c) {
                case '{': return parseObject();
                case '[': return parseArray();
                case '"': return Node.string(parseStringRaw());
                case 't': return parseLiteral("true", Node.bool(true));
                case 'f': return parseLiteral("false", Node.bool(false));
                case 'n': return parseLiteral("null", Node.NULL_INSTANCE);
                default:
                    if (c == '-' || (c >= '0' && c <= '9')) {
                        return parseNumber();
                    }
                    throw new JsonParseException("Unexpected character '" + c + "' while expecting a value", pos);
            }
        }

        private Node parseObject() {
            expect('{');
            Node node = new Node(Type.OBJECT);
            skipWhitespace();
            if (peekChar() == '}') {
                pos++;
                return node;
            }
            while (true) {
                skipWhitespace();
                if (peekChar() != '"') {
                    throw new JsonParseException("Expected string key in object", pos);
                }
                String key = parseStringRaw();
                skipWhitespace();
                expect(':');
                Node value = parseValue();
                node.objectValue.put(key, value);
                skipWhitespace();
                char c = nextChar();
                if (c == ',') {
                    continue;
                } else if (c == '}') {
                    break;
                } else {
                    throw new JsonParseException("Expected ',' or '}' in object", pos - 1);
                }
            }
            return node;
        }

        private Node parseArray() {
            expect('[');
            Node node = new Node(Type.ARRAY);
            skipWhitespace();
            if (peekChar() == ']') {
                pos++;
                return node;
            }
            while (true) {
                Node value = parseValue();
                node.arrayValue.add(value);
                skipWhitespace();
                char c = nextChar();
                if (c == ',') {
                    continue;
                } else if (c == ']') {
                    break;
                } else {
                    throw new JsonParseException("Expected ',' or ']' in array", pos - 1);
                }
            }
            return node;
        }

        private String parseStringRaw() {
            expect('"');
            StringBuilder sb = new StringBuilder();
            while (true) {
                if (pos >= len) {
                    throw new JsonParseException("Unterminated string literal", pos);
                }
                char c = src.charAt(pos++);
                if (c == '"') break;
                if (c == '\\') {
                    if (pos >= len) throw new JsonParseException("Unterminated escape sequence", pos);
                    char esc = src.charAt(pos++);
                    switch (esc) {
                        case '"': sb.append('"'); break;
                        case '\\': sb.append('\\'); break;
                        case '/': sb.append('/'); break;
                        case 'b': sb.append('\b'); break;
                        case 'f': sb.append('\f'); break;
                        case 'n': sb.append('\n'); break;
                        case 'r': sb.append('\r'); break;
                        case 't': sb.append('\t'); break;
                        case 'u':
                            if (pos + 4 > len) throw new JsonParseException("Invalid \\u escape sequence", pos);
                            String hex = src.substring(pos, pos + 4);
                            try {
                                sb.append((char) Integer.parseInt(hex, 16));
                            } catch (NumberFormatException e) {
                                throw new JsonParseException("Invalid \\u escape sequence: " + hex, pos);
                            }
                            pos += 4;
                            break;
                        default:
                            throw new JsonParseException("Invalid escape character: \\" + esc, pos - 1);
                    }
                } else if (c < 0x20) {
                    throw new JsonParseException("Unescaped control character in string", pos - 1);
                } else {
                    sb.append(c);
                }
            }
            return sb.toString();
        }

        private Node parseNumber() {
            int start = pos;
            boolean integral = true;
            if (peekChar() == '-') pos++;
            if (pos >= len || !isDigit(src.charAt(pos))) {
                throw new JsonParseException("Invalid number literal", pos);
            }
            if (src.charAt(pos) == '0') {
                pos++;
            } else {
                while (pos < len && isDigit(src.charAt(pos))) pos++;
            }
            if (pos < len && src.charAt(pos) == '.') {
                integral = false;
                pos++;
                if (pos >= len || !isDigit(src.charAt(pos))) {
                    throw new JsonParseException("Invalid number literal (digits expected after '.')", pos);
                }
                while (pos < len && isDigit(src.charAt(pos))) pos++;
            }
            if (pos < len && (src.charAt(pos) == 'e' || src.charAt(pos) == 'E')) {
                integral = false;
                pos++;
                if (pos < len && (src.charAt(pos) == '+' || src.charAt(pos) == '-')) pos++;
                if (pos >= len || !isDigit(src.charAt(pos))) {
                    throw new JsonParseException("Invalid number literal (digits expected in exponent)", pos);
                }
                while (pos < len && isDigit(src.charAt(pos))) pos++;
            }
            String numStr = src.substring(start, pos);
            double value = Double.parseDouble(numStr);
            // ถ้าเป็นจำนวนเต็มแต่เกินขอบเขต double-precision integer ที่ปลอดภัย ยังคงเก็บเป็น integral
            // เพื่อ serialize กลับโดยไม่มีจุดทศนิยม (คล้าย long)
            return Node.number(value, integral);
        }

        private Node parseLiteral(String literal, Node value) {
            if (pos + literal.length() > len || !src.regionMatches(pos, literal, 0, literal.length())) {
                throw new JsonParseException("Invalid literal, expected '" + literal + "'", pos);
            }
            pos += literal.length();
            return value;
        }

        private void skipWhitespace() {
            while (pos < len) {
                char c = src.charAt(pos);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    pos++;
                } else {
                    break;
                }
            }
        }

        private char peekChar() {
            if (pos >= len) throw new JsonParseException("Unexpected end of input", pos);
            return src.charAt(pos);
        }

        private char nextChar() {
            if (pos >= len) throw new JsonParseException("Unexpected end of input", pos);
            return src.charAt(pos++);
        }

        private void expect(char expected) {
            if (pos >= len || src.charAt(pos) != expected) {
                throw new JsonParseException("Expected '" + expected + "'", pos);
            }
            pos++;
        }

        private static boolean isDigit(char c) {
            return c >= '0' && c <= '9';
        }
    }
}