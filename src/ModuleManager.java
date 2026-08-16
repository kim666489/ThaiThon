import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.stream.Stream;

public class ModuleManager {
    public static Path MotherPath;
    public static String OSname;
    public static boolean debug;
    public static Path BuildPath;
    public static Path CompilerLibDir;
    public static Path ProjectRoot;
    public static Path ProjectLibFile;
    public static final String COMPILER_NAME = "ThaiThon";
    public static final String DEFAULT_GITHUB_BASE = "https://github.com";

    public static Map<String, String> ConfigMapping = Map.of(
        "-d", "client.config.debug",
        "-i", "client.config.input",
        "-o", "client.config.output",
        "-m", "client.config.mode"
    );
    public static Properties ConfigProp = new Properties();

    public static void main(String[] args) {
        try {
            MotherPath = detectProjectRoot();
            ProjectRoot = findProjectRootFromCwd();
            CompilerLibDir = MotherPath.resolve("lib");
            ProjectLibFile = ProjectRoot.resolve("lib.json");

            Files.createDirectories(CompilerLibDir);

            try (InputStream configStream = Files.newInputStream(Paths.get(MotherPath.toString(), "config", "client.properties"))) {
                ConfigProp.load(configStream);
            }

            ArrayList<String> parsedArgs = ReaderConfigMap(args);
            debug = SWtext(ConfigProp.getProperty("client.config.debug"), false);
            OSname = System.getProperty("os.name");

            if (debug) {
                System.out.println("OS: " + OSname);
                System.out.println("MotherPath: " + MotherPath);
                System.out.println("ProjectRoot: " + ProjectRoot);
                System.out.println("CompilerLibDir: " + CompilerLibDir);
            }

            if (parsedArgs.isEmpty()) {
                printHelp();
                return;
            }

            ExecuteCommands(parsedArgs);
        } catch (Exception e) {
            System.err.println("[Error] " + e.getMessage());
            if (debug) {
                e.printStackTrace(System.err);
            }
            System.exit(1);
        }
    }

    public static void ExecuteCommands(ArrayList<String> args) throws Exception {
        if (args.isEmpty()) {
            printHelp();
            return;
        }

        String command = args.get(0).toLowerCase(Locale.ROOT);
        switch (command) {
            case "install":
                installModule(args.subList(1, args.size()));
                break;
            case "uninstall":
            case "remove":
                removeModule(args.subList(1, args.size()));
                break;
            case "update":
                updateModule(args.subList(1, args.size()));
                break;
            case "search":
                searchModule(args.subList(1, args.size()));
                break;
            case "link":
                linkModule(args.subList(1, args.size()));
                break;
            case "list":
                listInstalledModules();
                break;
            case "help":
            case "--help":
            case "-h":
                printHelp();
                break;
            default:
                System.err.println("Unknown command: " + command);
                printHelp();
                System.exit(2);
                break;
        }
    }

    public static ArrayList<String> ReaderConfigMap(String[] args) {
        ArrayList<String> output = new ArrayList<>();

        for (String arg : args) {
            if (arg == null || arg.isEmpty()) continue;
            String[] parse = arg.split("=", 2);
            if (parse.length < 2) {
                output.add(parse[0]);
                continue;
            }

            String key = parse[0];
            String value = parse[1];
            if (!ConfigMapping.containsKey(key)) {
                output.add(arg);
                continue;
            }
            String propKey = ConfigMapping.get(key);
            if (propKey != null) {
                ConfigProp.setProperty(propKey, value);
            }
        }

        return output;
    }

    public static void printHelp() {
        System.out.println("ThaiThon ModuleManager");
        System.out.println("Usage:");
        System.out.println("  java -cp ./bin ModuleManager install <module> [version] [--repo=<github-url>] [--mode=permanent|project]");
        System.out.println("  java -cp ./bin ModuleManager update <module> [version] [--repo=<github-url>]");
        System.out.println("  java -cp ./bin ModuleManager uninstall <module>");
        System.out.println("  java -cp ./bin ModuleManager remove <module>");
        System.out.println("  java -cp ./bin ModuleManager search <keyword>");
        System.out.println("  java -cp ./bin ModuleManager link <module>");
        System.out.println("  java -cp ./bin ModuleManager list");
        System.out.println();
        System.out.println("Notes:");
        System.out.println("  - permanent mode installs into <compiler_root>/lib");
        System.out.println("  - project mode installs into <project_root>/lib and updates lib.json");
        System.out.println("  - module metadata must contain version and compilerVersion or compiler.version");
    }

    private static void installModule(List<String> args) throws Exception {
        if (args.isEmpty()) {
            throw new IllegalArgumentException("install requires module name");
        }

        String name = args.get(0);
        String version = null;
        String repoUrl = null;
        String mode = "permanent";

        for (int i = 1; i < args.size(); i++) {
            String arg = args.get(i);
            if (arg.startsWith("--repo=")) {
                repoUrl = arg.substring("--repo=".length()).trim();
            } else if (arg.startsWith("--mode=")) {
                mode = arg.substring("--mode=".length()).trim().toLowerCase(Locale.ROOT);
            } else if (arg.startsWith("--version=")) {
                version = arg.substring("--version=".length()).trim();
            } else if (arg.startsWith("-v")) {
                version = arg.substring(2).trim();
            } else if (isVersionToken(arg)) {
                version = arg;
            }
        }

        Path targetRoot = "project".equals(mode) ? ProjectRoot.resolve("lib") : CompilerLibDir;
        Files.createDirectories(targetRoot);

        ModuleMetadata metadata = fetchModuleMetadata(name, version, repoUrl);
        String requiredCompilerVersion = metadata.compilerVersion;
        if (!requiredCompilerVersion.isEmpty()) {
            validateCompilerVersion(requiredCompilerVersion, readCompilerVersionFromProject());
        }

        Path installDir = targetRoot.resolve(metadata.name);
        if (Files.exists(installDir)) {
            if (version == null || version.equals(metadata.version)) {
                System.out.println("Module already installed: " + metadata.name + " @ " + metadata.version);
                if ("project".equals(mode)) {
                    linkModuleToProject(metadata.name, installDir, metadata.version, requiredCompilerVersion);
                }
                return;
            }
            deleteRecursively(installDir);
        }

        copyDirectory(metadata.sourceDir, installDir);
        writeModuleIndex(metadata, installDir);

        if ("project".equals(mode)) {
            linkModuleToProject(metadata.name, installDir, metadata.version, requiredCompilerVersion);
        }

        System.out.println("Installed module: " + metadata.name + " @ " + metadata.version + " into " + installDir);
    }

    private static void updateModule(List<String> args) throws Exception {
        if (args.isEmpty()) {
            throw new IllegalArgumentException("update requires module name");
        }

        String name = args.get(0);
        String version = null;
        String repoUrl = null;

        for (int i = 1; i < args.size(); i++) {
            String arg = args.get(i);
            if (arg.startsWith("--repo=")) {
                repoUrl = arg.substring("--repo=".length()).trim();
            } else if (arg.startsWith("--version=")) {
                version = arg.substring("--version=".length()).trim();
            } else if (arg.startsWith("-v")) {
                version = arg.substring(2).trim();
            } else if (isVersionToken(arg)) {
                version = arg;
            }
        }

        Path moduleDir = findInstalledModuleDir(name);
        if (moduleDir == null) {
            System.out.println("Module not found locally: " + name + ". Running install instead.");
            installModule(args);
            return;
        }

        ModuleMetadata metadata = fetchModuleMetadata(name, version, repoUrl);
        validateCompilerVersion(metadata.compilerVersion, readCompilerVersionFromProject());

        deleteRecursively(moduleDir);
        Path installDir = CompilerLibDir.resolve(metadata.name);
        copyDirectory(metadata.sourceDir, installDir);
        writeModuleIndex(metadata, installDir);

        System.out.println("Updated module: " + metadata.name + " -> " + metadata.version);
    }

    private static void removeModule(List<String> args) throws Exception {
        if (args.isEmpty()) {
            throw new IllegalArgumentException("remove requires module name");
        }

        String name = args.get(0);
        Path moduleDir = findInstalledModuleDir(name);
        if (moduleDir == null) {
            System.out.println("Module not installed: " + name);
            return;
        }

        deleteRecursively(moduleDir);
        removeProjectLink(name);
        System.out.println("Removed module: " + name);
    }

    private static void searchModule(List<String> args) throws Exception {
        if (args.isEmpty()) {
            throw new IllegalArgumentException("search requires keyword");
        }

        String keyword = args.get(0);
        List<String> local = findInstalledMatches(keyword);
        if (!local.isEmpty()) {
            System.out.println("Local matches:");
            for (String name : local) {
                System.out.println("  - " + name);
            }
        } else {
            System.out.println("No local module matches keyword: " + keyword);
        }

        System.out.println();
        System.out.println("Remote GitHub search:");
        List<String> remote = searchGitHubRepositories(keyword);
        if (remote.isEmpty()) {
            System.out.println("  No remote result found for: " + keyword);
        } else {
            for (String item : remote) {
                System.out.println("  - " + item);
            }
        }
    }

    private static void linkModule(List<String> args) throws Exception {
        if (args.isEmpty()) {
            throw new IllegalArgumentException("link requires module name");
        }

        String name = args.get(0);
        Path installPath = findInstalledModuleDir(name);
        if (installPath == null) {
            throw new IllegalArgumentException("Module not installed: " + name + ". Run install first.");
        }

        Path metadataPath = installPath.resolve("module.json");
        if (!Files.exists(metadataPath)) {
            throw new IllegalArgumentException("module.json missing in installed module: " + name);
        }

        ModuleMetadata metadata = readInstalledMetadata(installPath);
        linkModuleToProject(metadata.name, installPath, metadata.version, metadata.compilerVersion);
        System.out.println("Linked module " + metadata.name + " to project: " + ProjectRoot);
    }

    private static void listInstalledModules() throws IOException {
        if (!Files.exists(CompilerLibDir)) {
            System.out.println("No library folder exists yet: " + CompilerLibDir);
            return;
        }

        try (Stream<Path> stream = Files.list(CompilerLibDir)) {
            List<Path> modules = stream.filter(Files::isDirectory).sorted(Comparator.comparing(path -> path.getFileName().toString())).toList();
            if (modules.isEmpty()) {
                System.out.println("No installed modules found under " + CompilerLibDir);
                return;
            }

            System.out.println("Installed modules:");
            for (Path p : modules) {
                Path metadata = p.resolve("module.json");
                String version = "unknown";
                if (Files.exists(metadata)) {
                    try {
                        ModuleMetadata md = readInstalledMetadata(p);
                        version = md.version;
                    } catch (Exception ignored) {
                    }
                }
                System.out.println("  - " + p.getFileName() + " @ " + version);
            }
        }
    }

    private static ModuleMetadata fetchModuleMetadata(String moduleName, String requestedVersion, String repoUrl) throws Exception {
        Path tempDir = Files.createTempDirectory("tt_module_");
        String cloneUrl = repoUrl;
        if (cloneUrl == null || cloneUrl.isBlank()) {
            cloneUrl = guessGitHubUrl(moduleName);
        }
        if (cloneUrl == null || cloneUrl.isBlank()) {
            throw new IllegalArgumentException("Module repository URL not provided and could not be guessed for: " + moduleName);
        }

        runGitCommand("clone", "--depth", "1", cloneUrl, tempDir.toString());
        Path repoRoot = findGitRepoRoot(tempDir);
        if (repoRoot == null) {
            throw new IllegalArgumentException("Git clone succeeded but repository root could not be resolved for: " + cloneUrl);
        }

        Path libJson = findFirstFile(repoRoot, "lib.json");
        if (libJson == null) {
            throw new IllegalArgumentException("Repository does not contain lib.json: " + cloneUrl);
        }

        Json.Node root = Json.parse(Files.readString(libJson, StandardCharsets.UTF_8));
        ModuleMetadata metadata = parseModuleMetadata(moduleName, requestedVersion, root, repoUrl);
        metadata.sourceDir = repoRoot;
        return metadata;
    }

    private static ModuleMetadata parseModuleMetadata(String moduleName, String requestedVersion, Json.Node root, String repoUrl) {
        String moduleKey = null;
        Json.Node moduleNode = null;

        if (root.isObject()) {
            if (root.has(moduleName)) {
                moduleKey = moduleName;
                moduleNode = root.get(moduleName);
            } else if (root.has("name") || root.has("version") || root.has("compilerVersion") || root.has("compiler")) {
                moduleKey = root.has("name") ? root.get("name").asString(moduleName) : moduleName;
                moduleNode = root;
            } else {
                for (String key : root.keys()) {
                    Json.Node candidate = root.get(key);
                    if (candidate.isObject() && (candidate.has("source") || candidate.has("functions") || candidate.has("declare") || candidate.has("version"))) {
                        moduleKey = key;
                        moduleNode = candidate;
                        break;
                    }
                }
            }
        }

        if (moduleNode == null || !moduleNode.isObject()) {
            throw new IllegalArgumentException("lib.json does not contain a valid module definition for: " + moduleName);
        }

        String recordedName = safeString(moduleNode.get("name"), moduleKey != null ? moduleKey : moduleName);
        String versionValue = safeString(moduleNode.get("version"), requestedVersion != null ? requestedVersion : "0.0.0");
        String compilerVersionValue = resolveCompilerVersion(moduleNode);
        if (requestedVersion != null && !requestedVersion.isBlank() && !requestedVersion.equals(versionValue)) {
            versionValue = requestedVersion;
        }

        ModuleMetadata metadata = new ModuleMetadata();
        metadata.name = recordedName;
        metadata.version = versionValue;
        metadata.compilerVersion = compilerVersionValue;
        metadata.repoUrl = repoUrl;
        return metadata;
    }

    private static String resolveCompilerVersion(Json.Node node) {
        if (node == null || !node.isObject()) {
            return "";
        }
        if (node.has("compilerVersion")) {
            return safeString(node.get("compilerVersion"), "");
        }
        if (node.has("compiler")) {
            Json.Node compiler = node.get("compiler");
            if (compiler.isObject()) {
                if (compiler.has("version")) {
                    return safeString(compiler.get("version"), "");
                }
                if (compiler.has("compilerVersion")) {
                    return safeString(compiler.get("compilerVersion"), "");
                }
            }
            return safeString(compiler, "");
        }
        return "";
    }

    private static void writeModuleIndex(ModuleMetadata md, Path installDir) throws IOException {
        Json.Node moduleInfo = Json.object()
            .put("name", md.name)
            .put("version", md.version)
            .put("compilerVersion", md.compilerVersion)
            .put("repoUrl", md.repoUrl == null ? "" : md.repoUrl)
            .put("installedAt", installDir.toString());
        Files.writeString(installDir.resolve("module.json"), moduleInfo.toPrettyString(2), StandardCharsets.UTF_8);
    }

    private static ModuleMetadata readInstalledMetadata(Path moduleDir) throws IOException {
        Path meta = moduleDir.resolve("module.json");
        if (!Files.exists(meta)) {
            throw new IllegalArgumentException("module.json not found in: " + moduleDir);
        }
        Json.Node node = Json.parse(Files.readString(meta, StandardCharsets.UTF_8));
        ModuleMetadata md = new ModuleMetadata();
        md.name = safeString(node.get("name"), moduleDir.getFileName().toString());
        md.version = safeString(node.get("version"), "0.0.0");
        md.compilerVersion = safeString(node.get("compilerVersion"), "");
        md.repoUrl = safeString(node.get("repoUrl"), "");
        return md;
    }

    private static void linkModuleToProject(String moduleName, Path installDir, String version, String compilerVersion) throws IOException {
        Files.createDirectories(ProjectRoot);
        Json.Node root = Files.exists(ProjectLibFile) ? Json.parse(Files.readString(ProjectLibFile, StandardCharsets.UTF_8)) : Json.object();
        if (!root.isObject()) {
            root = Json.object();
        }

        Json.Node entry = Json.object()
            .put("path", installDir.toString())
            .put("version", version)
            .put("compilerVersion", compilerVersion)
            .put("mode", "permanent");

        root.put(moduleName, entry);
        Files.writeString(ProjectLibFile, root.toPrettyString(2), StandardCharsets.UTF_8);
    }

    private static void removeProjectLink(String moduleName) throws IOException {
        if (!Files.exists(ProjectLibFile)) {
            return;
        }
        Json.Node root = Json.parse(Files.readString(ProjectLibFile, StandardCharsets.UTF_8));
        if (root.isObject() && root.has(moduleName)) {
            root.remove(moduleName);
            Files.writeString(ProjectLibFile, root.toPrettyString(2), StandardCharsets.UTF_8);
        }
    }

    private static List<String> findInstalledMatches(String keyword) throws IOException {
        List<String> result = new ArrayList<>();
        if (!Files.exists(CompilerLibDir)) {
            return result;
        }
        try (Stream<Path> stream = Files.list(CompilerLibDir)) {
            for (Path p : stream.filter(Files::isDirectory).toList()) {
                String name = p.getFileName().toString();
                if (name.toLowerCase(Locale.ROOT).contains(keyword.toLowerCase(Locale.ROOT))) {
                    result.add(name);
                }
            }
        }
        return result;
    }

    private static String guessGitHubUrl(String moduleName) {
        String clean = moduleName.trim();
        String lower = clean.toLowerCase(Locale.ROOT);
        if (lower.startsWith("https://") || lower.startsWith("http://") || lower.startsWith("git@")) {
            return clean;
        }
        if (clean.contains("/")) {
            return clean.endsWith(".git") ? clean : clean + ".git";
        }
        return DEFAULT_GITHUB_BASE + "/ThaiThon/" + clean + ".git";
    }

    private static List<String> searchGitHubRepositories(String keyword) throws Exception {
        String query = "ThaiThon+" + keyword.trim().replace(" ", "+");
        String url = "https://api.github.com/search/repositories?q=" + query + "&per_page=5";
        HttpClient client = HttpClient.newHttpClient();
        HttpRequest request = HttpRequest.newBuilder(URI.create(url))
            .header("Accept", "application/vnd.github+json")
            .header("User-Agent", "ThaiThon-ModuleManager")
            .build();

        HttpResponse<String> response = client.send(request, HttpResponse.BodyHandlers.ofString());
        if (response.statusCode() != 200) {
            return List.of();
        }

        Json.Node root = Json.parse(response.body());
        List<String> names = new ArrayList<>();
        if (!root.isObject() || !root.has("items") || !root.get("items").isArray()) {
            return names;
        }

        for (Json.Node item : root.get("items")) {
            if (item.isObject()) {
                names.add(item.get("full_name").asString() + " -> " + item.get("html_url").asString());
            }
        }
        return names;
    }

    private static void runGitCommand(String... args) throws IOException, InterruptedException {
        List<String> command = new ArrayList<>();
        command.add("git");
        for (String arg : args) {
            command.add(arg);
        }
        ProcessBuilder builder = new ProcessBuilder(command);
        builder.redirectErrorStream(true);
        Process process = builder.start();
        String output = readProcessOutput(process);
        int exitCode = process.waitFor();
        if (exitCode != 0) {
            throw new IOException("Git command failed (exit " + exitCode + "): " + output);
        }
    }

    private static String readProcessOutput(Process process) throws IOException {
        StringBuilder sb = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line).append(System.lineSeparator());
            }
        }
        return sb.toString();
    }

    private static Path findGitRepoRoot(Path start) throws IOException {
        Path current = start;
        while (current != null && Files.exists(current)) {
            Path gitDir = current.resolve(".git");
            if (Files.exists(gitDir)) {
                return current;
            }
            current = current.getParent();
        }
        return null;
    }

    private static Path findFirstFile(Path root, String filename) throws IOException {
        try (Stream<Path> stream = Files.walk(root)) {
            return stream.filter(p -> p.getFileName() != null && p.getFileName().toString().equals(filename))
                .findFirst()
                .orElse(null);
        }
    }

    private static Path findInstalledModuleDir(String moduleName) throws IOException {
        if (!Files.exists(CompilerLibDir)) {
            return null;
        }
        try (Stream<Path> stream = Files.list(CompilerLibDir)) {
            return stream.filter(Files::isDirectory)
                .filter(p -> p.getFileName().toString().equalsIgnoreCase(moduleName))
                .findFirst()
                .orElse(null);
        }
    }

    private static void validateCompilerVersion(String required, String actual) {
        if (required == null || required.isBlank()) {
            return;
        }
        String requiredNorm = normalizeVersion(required);
        String actualNorm = normalizeVersion(actual);
        if (requiredNorm.isEmpty() || actualNorm.isEmpty()) {
            return;
        }
        if (compareVersions(actualNorm, requiredNorm) < 0) {
            throw new IllegalArgumentException("Compiler version mismatch. Required: " + required + ", current: " + actual);
        }
    }

    private static String normalizeVersion(String value) {
        if (value == null) {
            return "";
        }
        String cleaned = value.trim();
        if (cleaned.startsWith("v") || cleaned.startsWith("V")) {
            cleaned = cleaned.substring(1);
        }
        if (cleaned.startsWith(">=") || cleaned.startsWith("<=") || cleaned.startsWith("==") || cleaned.startsWith("!=") || cleaned.startsWith(">") || cleaned.startsWith("<")) {
            cleaned = cleaned.substring(2).trim();
            if (cleaned.isEmpty()) {
                return "";
            }
        }
        cleaned = cleaned.replaceAll("[^0-9.]", "");
        while (cleaned.endsWith(".")) {
            cleaned = cleaned.substring(0, cleaned.length() - 1);
        }
        return cleaned;
    }

    private static int compareVersions(String left, String right) {
        String[] a = left.split("\\.");
        String[] b = right.split("\\.");
        int maxLen = Math.max(a.length, b.length);
        for (int i = 0; i < maxLen; i++) {
            int av = i < a.length && !a[i].isBlank() ? Integer.parseInt(a[i]) : 0;
            int bv = i < b.length && !b[i].isBlank() ? Integer.parseInt(b[i]) : 0;
            if (av < bv) return -1;
            if (av > bv) return 1;
        }
        return 0;
    }

    private static String readCompilerVersionFromProject() {
        Path versionPath = MotherPath.resolve("config.json");
        if (!Files.exists(versionPath)) {
            return "0.0.0";
        }
        try {
            Json.Node root = Json.parse(Files.readString(versionPath, StandardCharsets.UTF_8));
            if (root.isObject() && root.has("version")) {
                return safeString(root.get("version"), "0.0.0");
            }
        } catch (Exception ignored) {
        }
        return "0.0.0";
    }

    private static Path detectProjectRoot() {
        try {
            Path candidate = Paths.get(ModuleManager.class.getProtectionDomain().getCodeSource().getLocation().toURI()).getParent().getParent().getParent();
            if (Files.exists(candidate.resolve("src")) && Files.exists(candidate.resolve("lib"))) {
                return candidate;
            }
        } catch (Exception ignored) {
        }
        return Paths.get(System.getProperty("user.dir", ".")).toAbsolutePath().normalize();
    }

    private static Path findProjectRootFromCwd() {
        Path current = Paths.get(System.getProperty("user.dir", ".")).toAbsolutePath().normalize();
        if (Files.exists(current.resolve("src")) && Files.exists(current.resolve("lib"))) {
            return current;
        }
        Path parent = current.getParent();
        while (parent != null) {
            if (Files.exists(parent.resolve("src")) && Files.exists(parent.resolve("lib"))) {
                return parent;
            }
            parent = parent.getParent();
        }
        return current;
    }

    private static void copyDirectory(Path source, Path target) throws IOException {
        if (!Files.exists(source)) {
            throw new IOException("Source directory does not exist: " + source);
        }
        Files.createDirectories(target);
        try (Stream<Path> stream = Files.walk(source)) {
            for (Path srcFile : stream.toList()) {
                Path relative = source.relativize(srcFile);
                Path dest = target.resolve(relative.toString());
                if (Files.isDirectory(srcFile)) {
                    Files.createDirectories(dest);
                } else {
                    Files.createDirectories(dest.getParent());
                    Files.copy(srcFile, dest, StandardCopyOption.REPLACE_EXISTING);
                }
            }
        }
    }

    private static void deleteRecursively(Path path) throws IOException {
        if (!Files.exists(path)) {
            return;
        }
        try (Stream<Path> stream = Files.walk(path)) {
            stream.sorted(Comparator.reverseOrder()).forEach(p -> {
                try {
                    Files.deleteIfExists(p);
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });
        } catch (RuntimeException e) {
            if (e.getCause() instanceof IOException io) {
                throw io;
            }
            throw e;
        }
    }

    private static boolean isVersionToken(String text) {
        if (text == null || text.isBlank()) {
            return false;
        }
        String cleaned = text.trim();
        return cleaned.matches("[vV]?[0-9]+(\\.[0-9]+)*") || cleaned.matches("[<>!=]=?[0-9]+(\\.[0-9]+)*");
    }

    private static String safeString(Json.Node node, String defaultValue) {
        if (node == null || node.isNull()) {
            return defaultValue;
        }
        return node.asString(defaultValue);
    }

    private static class ModuleMetadata {
        public String name;
        public String version;
        public String compilerVersion;
        public String repoUrl;
        public Path sourceDir;
    }

    public static boolean is_Digital(String text) {
        try {
            Integer.parseInt(text);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    public static boolean is_Boolean(String text) {
        return "true".equals(text) || "false".equals(text);
    }

    public static String SWtext(String text) {
        return text;
    }

    public static Boolean SWtext(String text, Boolean defaultValue) {
        if (text == null) return defaultValue;
        if (is_Boolean(text)) {
            return Boolean.parseBoolean(text.trim());
        } else {
            System.err.println("[Error] Is " + text + " not boolean.");
            System.exit(1);
            return defaultValue;
        }
    }

    public static Integer SWtext(String text, Integer defaultValue) {
        if (text == null) return defaultValue;
        try {
            return Integer.parseInt(text.trim());
        } catch (NumberFormatException e) {
            System.err.println("[Error] Is " + text + " not integer.");
            System.exit(1);
            return defaultValue;
        }
    }
}
