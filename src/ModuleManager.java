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
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
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

    // BUG-09 fix: hard ceiling on how long any git subprocess may run.
    private static final long GIT_TIMEOUT_SECONDS = 60;

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
        System.out.println("  java -cp ./bin ModuleManager install <module> [version] --repo=<git-url> [--mode=permanent|project]");
        System.out.println("  java -cp ./bin ModuleManager update <module> [version] [--repo=<git-url>]");
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
        System.out.println("  - --repo=<owner/repo or full git URL> is required unless <module> already looks like one");
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

        // BUG-01 fix: fetchModuleMetadata's temp clone directory is always
        // cleaned up in the finally block below, on every code path
        // (already-installed short circuit included).
        ModuleMetadata metadata = fetchModuleMetadata(name, version, repoUrl);
        try {
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
                // BUG-04 fix: swap-then-delete instead of delete-then-copy,
                // so a failed copy can never leave the module missing.
                replaceInstalledModule(installDir, metadata);
                if ("project".equals(mode)) {
                    linkModuleToProject(metadata.name, installDir, metadata.version, requiredCompilerVersion);
                }
                System.out.println("Installed module: " + metadata.name + " @ " + metadata.version + " into " + installDir);
                return;
            }

            copyDirectoryExcludingGit(metadata.sourceDir, installDir);
            writeModuleIndex(metadata, installDir);

            if ("project".equals(mode)) {
                linkModuleToProject(metadata.name, installDir, metadata.version, requiredCompilerVersion);
            }

            System.out.println("Installed module: " + metadata.name + " @ " + metadata.version + " into " + installDir);
        } finally {
            cleanupTempDir(metadata);
        }
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
            // NOTE: --mode= is intentionally not parsed here (BUG-12 fix).
            // We now update the module in place, in whichever root
            // (compiler-permanent or project-local) it was actually found,
            // so the caller does not need to (and cannot mistakenly)
            // redirect an update to the wrong root via --mode=.
        }

        // BUG-03 fix: search both the compiler-permanent lib/ and the
        // project-local lib/ before giving up.
        Path moduleDir = findInstalledModuleDir(name);
        if (moduleDir == null) {
            System.out.println("Module not found locally: " + name + ". Running install instead.");
            installModule(args);
            return;
        }

        ModuleMetadata metadata = fetchModuleMetadata(name, version, repoUrl);
        try {
            validateCompilerVersion(metadata.compilerVersion, readCompilerVersionFromProject());

            // BUG-12 fix: install back into the same root the module was
            // actually found in, instead of hard-coding CompilerLibDir.
            replaceInstalledModule(moduleDir, metadata);

            System.out.println("Updated module: " + metadata.name + " -> " + metadata.version);
        } finally {
            cleanupTempDir(metadata);
        }
    }

    /**
     * BUG-04 fix: replaces an installed module directory with a freshly
     * fetched one without ever leaving the module in a half-deleted state.
     * The old copy is moved aside first; it is only deleted once the new
     * copy has been written successfully, and is restored automatically
     * if anything goes wrong.
     */
    private static void replaceInstalledModule(Path installDir, ModuleMetadata metadata) throws IOException {
        Path backupDir = null;
        boolean oldMoved = false;
        try {
            if (Files.exists(installDir)) {
                backupDir = installDir.resolveSibling(installDir.getFileName() + ".bak_" + System.nanoTime());
                Files.move(installDir, backupDir);
                oldMoved = true;
            }

            copyDirectoryExcludingGit(metadata.sourceDir, installDir);
            writeModuleIndex(metadata, installDir);

            if (oldMoved) {
                deleteRecursively(backupDir);
            }
        } catch (IOException e) {
            // Roll back: if the new copy didn't fully land, restore the backup.
            try {
                if (oldMoved && backupDir != null && Files.exists(backupDir)) {
                    deleteRecursively(installDir);
                    Files.move(backupDir, installDir);
                }
            } catch (IOException rollbackFailure) {
                e.addSuppressed(rollbackFailure);
            }
            throw e;
        }
    }

    private static void removeModule(List<String> args) throws Exception {
        if (args.isEmpty()) {
            throw new IllegalArgumentException("remove requires module name");
        }

        String name = args.get(0);
        // BUG-03 fix: search both compiler-permanent and project-local lib/.
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
        // BUG-03 fix: search both compiler-permanent and project-local lib/.
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
        boolean anyRoot = Files.exists(CompilerLibDir) || Files.exists(ProjectRoot.resolve("lib"));
        if (!anyRoot) {
            System.out.println("No library folder exists yet: " + CompilerLibDir);
            return;
        }

        boolean printedAny = false;
        printedAny |= listModulesUnder(CompilerLibDir, "permanent");
        Path projectLibDir = ProjectRoot.resolve("lib");
        if (!projectLibDir.equals(CompilerLibDir)) {
            printedAny |= listModulesUnder(projectLibDir, "project");
        }

        if (!printedAny) {
            System.out.println("No installed modules found under " + CompilerLibDir + " or " + projectLibDir);
        }
    }

    private static boolean listModulesUnder(Path root, String label) throws IOException {
        if (!Files.exists(root)) {
            return false;
        }
        try (Stream<Path> stream = Files.list(root)) {
            List<Path> modules = stream.filter(Files::isDirectory).sorted(Comparator.comparing(path -> path.getFileName().toString())).toList();
            if (modules.isEmpty()) {
                return false;
            }

            System.out.println("Installed modules (" + label + " @ " + root + "):");
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
            return true;
        }
    }

    private static ModuleMetadata fetchModuleMetadata(String moduleName, String requestedVersion, String repoUrl) throws Exception {
        Path tempDir = Files.createTempDirectory("tt_module_");
        boolean success = false;
        try {
            String cloneUrl = repoUrl;
            if (cloneUrl == null || cloneUrl.isBlank()) {
                cloneUrl = guessGitHubUrl(moduleName);
            }

            // BUG-02 fix: when a specific version is requested we cannot use
            // a shallow (--depth 1) clone, because that only ever contains
            // the tip of the default branch and no other tags/branches to
            // check out. Do a full clone in that case so the requested ref
            // can actually be resolved.
            if (requestedVersion != null && !requestedVersion.isBlank()) {
                runGitCommand(tempDir, "clone", cloneUrl, tempDir.toString());
            } else {
                runGitCommand(tempDir, "clone", "--depth", "1", cloneUrl, tempDir.toString());
            }

            Path repoRoot = findGitRepoRoot(tempDir);
            if (repoRoot == null) {
                throw new IllegalArgumentException("Git clone succeeded but repository root could not be resolved for: " + cloneUrl);
            }

            // BUG-02 fix: actually check out the requested version instead of
            // just relabeling whatever HEAD happened to contain.
            if (requestedVersion != null && !requestedVersion.isBlank()) {
                checkoutRequestedVersion(repoRoot, requestedVersion);
            }

            Path libJson = findFirstFile(repoRoot, "lib.json");
            if (libJson == null) {
                throw new IllegalArgumentException("Repository does not contain lib.json: " + cloneUrl);
            }

            Json.Node root = Json.parse(Files.readString(libJson, StandardCharsets.UTF_8));
            ModuleMetadata metadata = parseModuleMetadata(moduleName, requestedVersion, root, repoUrl);
            metadata.sourceDir = repoRoot;
            metadata.tempDir = tempDir;
            success = true;
            return metadata;
        } finally {
            // BUG-01 fix: if anything above failed before we could hand the
            // temp dir off to the caller, clean it up right away instead of
            // leaking it. On the success path, the caller (installModule /
            // updateModule) is responsible for deleting it via
            // cleanupTempDir() once it has finished copying out of it.
            if (!success) {
                try {
                    deleteRecursively(tempDir);
                } catch (IOException ignored) {
                }
            }
        }
    }

    /**
     * BUG-02 fix helper: tries a handful of reasonable ref spellings
     * ("1.2.3", "v1.2.3") as both tags and branches. Throws a clear error
     * if none of them resolve, rather than silently keeping whatever ref
     * the clone happened to check out.
     */
    private static void checkoutRequestedVersion(Path repoRoot, String requestedVersion) throws IOException, InterruptedException {
        String trimmed = requestedVersion.trim();
        List<String> candidates = new ArrayList<>();
        candidates.add(trimmed);
        if (!trimmed.startsWith("v") && !trimmed.startsWith("V")) {
            candidates.add("v" + trimmed);
        } else {
            candidates.add(trimmed.substring(1));
        }

        for (String ref : candidates) {
            if (tryCheckout(repoRoot, ref)) {
                return;
            }
        }

        throw new IllegalArgumentException(
            "Requested version \"" + requestedVersion + "\" was not found as a git tag or branch in the module repository. " +
            "Tried: " + String.join(", ", candidates));
    }

    private static boolean tryCheckout(Path repoRoot, String ref) {
        try {
            runGitCommand(repoRoot, "checkout", ref);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * BUG-01 fix: deletes the temp clone directory associated with a
     * ModuleMetadata, if any. Safe to call even if the metadata is null
     * or the temp dir was already cleaned up.
     */
    private static void cleanupTempDir(ModuleMetadata metadata) {
        if (metadata == null || metadata.tempDir == null) {
            return;
        }
        try {
            deleteRecursively(metadata.tempDir);
        } catch (IOException e) {
            if (debug) {
                System.err.println("[Warn] Failed to clean up temp directory " + metadata.tempDir + ": " + e.getMessage());
            }
        }
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
                // BUG-11 fix: only auto-select a fallback entry when it is
                // unambiguous (exactly one module-like entry present).
                // Multiple candidates now hard-fail instead of silently
                // picking the first one, which could install the wrong
                // module without any warning.
                List<String> candidates = new ArrayList<>();
                for (String key : root.keys()) {
                    Json.Node candidate = root.get(key);
                    if (candidate.isObject() && (candidate.has("source") || candidate.has("functions") || candidate.has("declare") || candidate.has("version"))) {
                        candidates.add(key);
                    }
                }
                if (candidates.size() == 1) {
                    moduleKey = candidates.get(0);
                    moduleNode = root.get(moduleKey);
                    System.err.println("[Warn] lib.json key \"" + moduleKey + "\" does not match requested module name \"" +
                        moduleName + "\"; using it because it is the only module-like entry found.");
                } else if (candidates.size() > 1) {
                    throw new IllegalArgumentException(
                        "lib.json defines multiple modules (" + String.join(", ", candidates) +
                        ") and none match the requested name \"" + moduleName + "\". " +
                        "Specify the exact module name that matches a key in lib.json.");
                }
            }
        }

        if (moduleNode == null || !moduleNode.isObject()) {
            throw new IllegalArgumentException("lib.json does not contain a valid module definition for: " + moduleName);
        }

        String recordedName = safeString(moduleNode.get("name"), moduleKey != null ? moduleKey : moduleName);
        String declaredVersion = safeString(moduleNode.get("version"), "");
        String compilerVersionValue = resolveCompilerVersion(moduleNode);

        // BUG-02 fix: the version we record now reflects what was actually
        // checked out (checkoutRequestedVersion already ran before this is
        // called, or no specific version was requested at all). We prefer
        // the version lib.json itself declares for the checked-out ref; we
        // only fall back to the caller-requested string if lib.json has no
        // version field to report, and in that case we say so explicitly.
        String versionValue;
        if (!declaredVersion.isBlank()) {
            versionValue = declaredVersion;
            if (requestedVersion != null && !requestedVersion.isBlank() &&
                !normalizeVersion(requestedVersion).equals(normalizeVersion(declaredVersion))) {
                System.err.println("[Warn] Requested version \"" + requestedVersion +
                    "\" but the checked-out lib.json reports version \"" + declaredVersion +
                    "\"; using the value from lib.json.");
            }
        } else if (requestedVersion != null && !requestedVersion.isBlank()) {
            versionValue = requestedVersion;
            System.err.println("[Warn] lib.json for module \"" + recordedName +
                "\" has no version field; recording the requested version \"" + requestedVersion +
                "\" as-is (unverified).");
        } else {
            versionValue = "0.0.0";
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
        result.addAll(findInstalledMatchesUnder(CompilerLibDir, keyword));
        Path projectLibDir = ProjectRoot.resolve("lib");
        if (!projectLibDir.equals(CompilerLibDir)) {
            result.addAll(findInstalledMatchesUnder(projectLibDir, keyword));
        }
        return result;
    }

    private static List<String> findInstalledMatchesUnder(Path root, String keyword) throws IOException {
        List<String> result = new ArrayList<>();
        if (!Files.exists(root)) {
            return result;
        }
        try (Stream<Path> stream = Files.list(root)) {
            for (Path p : stream.filter(Files::isDirectory).toList()) {
                String name = p.getFileName().toString();
                if (name.toLowerCase(Locale.ROOT).contains(keyword.toLowerCase(Locale.ROOT))) {
                    result.add(name);
                }
            }
        }
        return result;
    }

    /**
     * BUG-08 fix: no longer guesses a nonexistent central "ThaiThon" GitHub
     * org. A bare module name (no "/", no scheme) is no longer enough
     * information to resolve a repository, so we now fail fast with a
     * clear, actionable error instead of silently building a URL that is
     * almost guaranteed to 404.
     */
    private static String guessGitHubUrl(String moduleName) {
        String clean = moduleName.trim();
        String lower = clean.toLowerCase(Locale.ROOT);
        if (lower.startsWith("https://") || lower.startsWith("http://") || lower.startsWith("git@")) {
            return clean;
        }
        if (clean.contains("/")) {
            // Treat as "owner/repo" shorthand against the default git host.
            return clean.endsWith(".git") ? (DEFAULT_GITHUB_BASE + "/" + clean) : (DEFAULT_GITHUB_BASE + "/" + clean + ".git");
        }
        throw new IllegalArgumentException(
            "Cannot resolve a repository for module \"" + moduleName + "\": no --repo=<url> was given and " +
            "\"" + moduleName + "\" is not an \"owner/repo\" shorthand or a full git URL. " +
            "Pass --repo=<git-url> (e.g. --repo=https://github.com/<owner>/" + moduleName + ".git) explicitly.");
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

    /**
     * BUG-09 fix: runs a git subprocess with a hard timeout and with
     * GIT_TERMINAL_PROMPT disabled, so an auth prompt on a private repo
     * fails fast instead of hanging forever with no feedback.
     * The command now also accepts a working directory, since checkout
     * needs to run inside the already-cloned repo rather than cloning
     * into a fresh temp dir.
     */
    private static void runGitCommand(Path workingDir, String... args) throws IOException, InterruptedException {
        List<String> command = new ArrayList<>();
        command.add("git");
        for (String arg : args) {
            command.add(arg);
        }
        ProcessBuilder builder = new ProcessBuilder(command);
        builder.redirectErrorStream(true);
        if (workingDir != null && Files.exists(workingDir)) {
            builder.directory(workingDir.toFile());
        }
        builder.environment().put("GIT_TERMINAL_PROMPT", "0");

        Process process = builder.start();
        String output = readProcessOutput(process);
        boolean finished = process.waitFor(GIT_TIMEOUT_SECONDS, TimeUnit.SECONDS);
        if (!finished) {
            process.destroyForcibly();
            throw new IOException("Git command timed out after " + GIT_TIMEOUT_SECONDS + "s: git " + String.join(" ", args));
        }
        int exitCode = process.exitValue();
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

    /**
     * BUG-03 fix: looks in both the compiler-permanent lib/ directory and
     * the project-local lib/ directory, so uninstall/update/link can find
     * modules regardless of which mode they were installed with. If a
     * module of the same name exists in both roots, the project-local copy
     * takes precedence (it is the one the current project would actually
     * use), and a warning is printed so the ambiguity is visible.
     */
    private static Path findInstalledModuleDir(String moduleName) throws IOException {
        Path projectHit = findInstalledModuleDirUnder(ProjectRoot.resolve("lib"), moduleName);
        Path compilerHit = findInstalledModuleDirUnder(CompilerLibDir, moduleName);

        if (projectHit != null && compilerHit != null) {
            System.err.println("[Warn] Module \"" + moduleName + "\" is installed both in the project (" +
                projectHit + ") and permanently (" + compilerHit + "). Using the project-local copy.");
            return projectHit;
        }
        return projectHit != null ? projectHit : compilerHit;
    }

    private static Path findInstalledModuleDirUnder(Path root, String moduleName) throws IOException {
        if (root == null || !Files.exists(root)) {
            return null;
        }
        try (Stream<Path> stream = Files.list(root)) {
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

    /**
     * BUG-06 fix: previously this stripped every non-digit/non-dot
     * character with a single regex pass, which silently glued together
     * unrelated numeric segments (e.g. "1.0.0-beta1" -> "1.0.01" instead of
     * "1.0.0"). It also mishandled single-character comparison operators
     * ("&gt;1.0.0" incorrectly dropped the leading digit because it always
     * cut 2 characters off, even for a 1-character operator). Both are
     * fixed below: operators are stripped by their own length, and any
     * pre-release/build suffix is dropped as a whole instead of being
     * character-filtered into the numeric part.
     */
    private static String normalizeVersion(String value) {
        if (value == null) {
            return "";
        }
        String cleaned = value.trim();
        if (cleaned.startsWith("v") || cleaned.startsWith("V")) {
            cleaned = cleaned.substring(1);
        }
        if (cleaned.startsWith(">=") || cleaned.startsWith("<=") || cleaned.startsWith("==") || cleaned.startsWith("!=")) {
            cleaned = cleaned.substring(2).trim();
        } else if (cleaned.startsWith(">") || cleaned.startsWith("<")) {
            cleaned = cleaned.substring(1).trim();
        }
        if (cleaned.isEmpty()) {
            return "";
        }
        // Keep only the leading dotted-numeric run; anything after the
        // first non [0-9.] character (pre-release tag, build metadata,
        // stray text) is dropped as a whole rather than filtered
        // character-by-character.
        Matcher m = Pattern.compile("^[0-9]+(\\.[0-9]+)*").matcher(cleaned);
        if (m.find()) {
            String result = m.group();
            while (result.endsWith(".")) {
                result = result.substring(0, result.length() - 1);
            }
            return result;
        }
        return "";
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

    /**
     * BUG-07 fix: the previous implementation always walked exactly three
     * parent directories up from the code source location, which only
     * happened to work for one specific "bin/ModuleManager.class" layout
     * and would silently fall back to the current working directory (with
     * no warning at all, even in debug mode) for any other layout (e.g.
     * running from a jar). This version instead walks upward a bounded
     * number of levels and checks for a much less ambiguous marker
     * (src/ + config/client.properties, which only the compiler root has),
     * and it logs a warning when it has to fall back.
     */
    private static Path detectProjectRoot() {
        try {
            Path codeLocation = Paths.get(ModuleManager.class.getProtectionDomain().getCodeSource().getLocation().toURI());
            Path start = Files.isRegularFile(codeLocation) ? codeLocation.getParent() : codeLocation;
            Path candidate = start;
            for (int depth = 0; depth < 6 && candidate != null; depth++) {
                if (looksLikeCompilerRoot(candidate)) {
                    return candidate;
                }
                candidate = candidate.getParent();
            }
        } catch (Exception ignored) {
        }

        Path fallback = Paths.get(System.getProperty("user.dir", ".")).toAbsolutePath().normalize();
        System.err.println("[Warn] Could not reliably detect the ThaiThon compiler root from the running class location; " +
            "falling back to the current working directory: " + fallback);
        return fallback;
    }

    private static boolean looksLikeCompilerRoot(Path candidate) {
        return Files.exists(candidate.resolve("src")) &&
            Files.exists(candidate.resolve("config").resolve("client.properties"));
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

    /**
     * BUG-05 fix: copyDirectory previously walked and copied the entire
     * source tree including ".git/", bloating every installed module with
     * the full clone's object database and risking nested-git-repo issues
     * if the compiler project itself is version controlled. This variant
     * skips any path whose relative form starts with ".git".
     */
    private static void copyDirectoryExcludingGit(Path source, Path target) throws IOException {
        if (!Files.exists(source)) {
            throw new IOException("Source directory does not exist: " + source);
        }
        Files.createDirectories(target);
        try (Stream<Path> stream = Files.walk(source)) {
            for (Path srcFile : stream.toList()) {
                Path relative = source.relativize(srcFile);
                String relativeStr = relative.toString();
                if (relativeStr.equals(".git") || relativeStr.startsWith(".git" + java.io.File.separator)) {
                    continue;
                }
                Path dest = target.resolve(relativeStr);
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
        // BUG-01 fix: tracks the temp clone directory so the caller can
        // (and must) clean it up once it is done reading from sourceDir.
        public Path tempDir;
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