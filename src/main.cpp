#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace {
using Value =
    std::variant<std::string, std::uint64_t, bool, std::vector<std::string>>;

struct Config {
  struct Dependency {
    std::string Name;
    std::string Repository;
    std::string Revision;
  };

  std::string Name;
  std::string Version;
  fs::path Entry;
  std::string Compiler = "kelyra";
  fs::path Output;
  fs::path PackageOutput;
  unsigned Optimization = 0;
  unsigned SafeLevel = 0;
  std::vector<std::string> CSources;
  std::vector<std::string> CArguments;
  std::vector<std::string> ModulePaths;
  std::vector<std::string> TestSources;
  std::vector<Dependency> Dependencies;
};

bool ValidName(std::string_view Name);

bool SafeRelativePath(const fs::path &Path) {
  if (Path.empty() || Path.is_absolute())
    return false;
  for (const auto &Part : Path)
    if (Part == "..")
      return false;
  return true;
}

std::string Trim(std::string_view Text) {
  while (!Text.empty() &&
         std::isspace(static_cast<unsigned char>(Text.front())))
    Text.remove_prefix(1);
  while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.back())))
    Text.remove_suffix(1);
  return std::string(Text);
}

std::string StripComment(std::string_view Line) {
  bool Quoted = false;
  bool Escaped = false;
  for (std::size_t I = 0; I < Line.size(); ++I) {
    if (Escaped) {
      Escaped = false;
      continue;
    }
    if (Quoted && Line[I] == '\\') {
      Escaped = true;
      continue;
    }
    if (Line[I] == '"')
      Quoted = !Quoted;
    else if (Line[I] == '#' && !Quoted)
      return std::string(Line.substr(0, I));
  }
  return std::string(Line);
}

std::string ParseString(std::string_view Text) {
  Text = std::string_view(Text.data(), Text.size());
  if (Text.size() < 2 || Text.front() != '"' || Text.back() != '"')
    throw std::runtime_error("expected a quoted string");
  std::string Result;
  for (std::size_t I = 1; I + 1 < Text.size(); ++I) {
    if (Text[I] != '\\') {
      Result.push_back(Text[I]);
      continue;
    }
    if (++I + 1 >= Text.size())
      throw std::runtime_error("unterminated string escape");
    const char Escaped = Text[I];
    if (Escaped == 'n')
      Result.push_back('\n');
    else if (Escaped == 'r')
      Result.push_back('\r');
    else if (Escaped == 't')
      Result.push_back('\t');
    else if (Escaped == '\\' || Escaped == '"')
      Result.push_back(Escaped);
    else
      throw std::runtime_error("unsupported string escape");
  }
  return Result;
}

std::vector<std::string> ParseArray(std::string_view Text) {
  if (Text.size() < 2 || Text.front() != '[' || Text.back() != ']')
    throw std::runtime_error("expected an array");
  Text.remove_prefix(1);
  Text.remove_suffix(1);
  std::vector<std::string> Result;
  while (!Trim(Text).empty()) {
    Text = std::string_view(Text.data(), Text.size());
    while (!Text.empty() &&
           std::isspace(static_cast<unsigned char>(Text.front())))
      Text.remove_prefix(1);
    if (Text.empty() || Text.front() != '"')
      throw std::runtime_error("arrays must contain strings");
    bool Escaped = false;
    std::size_t End = 1;
    for (; End < Text.size(); ++End) {
      if (!Escaped && Text[End] == '"')
        break;
      if (!Escaped && Text[End] == '\\')
        Escaped = true;
      else
        Escaped = false;
    }
    if (End == Text.size())
      throw std::runtime_error("unterminated array string");
    Result.push_back(ParseString(Text.substr(0, End + 1)));
    Text.remove_prefix(End + 1);
    while (!Text.empty() &&
           std::isspace(static_cast<unsigned char>(Text.front())))
      Text.remove_prefix(1);
    if (Text.empty())
      break;
    if (Text.front() != ',')
      throw std::runtime_error("expected ',' in array");
    Text.remove_prefix(1);
  }
  return Result;
}

Value ParseValue(std::string_view Text) {
  const auto Clean = Trim(Text);
  if (Clean.empty())
    throw std::runtime_error("missing value");
  if (Clean.front() == '"')
    return ParseString(Clean);
  if (Clean.front() == '[')
    return ParseArray(Clean);
  if (Clean == "true")
    return true;
  if (Clean == "false")
    return false;
  std::uint64_t Number = 0;
  const auto Parsed =
      std::from_chars(Clean.data(), Clean.data() + Clean.size(), Number);
  if (Parsed.ec != std::errc() || Parsed.ptr != Clean.data() + Clean.size())
    throw std::runtime_error("unsupported value");
  return Number;
}

std::map<std::string, Value> ParseToml(const fs::path &Path) {
  std::ifstream Input(Path);
  if (!Input)
    throw std::runtime_error("cannot open " + Path.string());
  std::map<std::string, Value> Values;
  std::string Section;
  std::string Line;
  unsigned LineNumber = 0;
  while (std::getline(Input, Line)) {
    ++LineNumber;
    const auto Clean = Trim(StripComment(Line));
    if (Clean.empty())
      continue;
    try {
      if (Clean.front() == '[') {
        if (Clean.size() < 3 || Clean.back() != ']')
          throw std::runtime_error("invalid table header");
        Section = Trim(std::string_view(Clean).substr(1, Clean.size() - 2));
        if (Section.empty())
          throw std::runtime_error("empty table name");
        continue;
      }
      const auto Equal = Clean.find('=');
      if (Equal == std::string::npos)
        throw std::runtime_error("expected '='");
      const auto Key = Trim(std::string_view(Clean).substr(0, Equal));
      if (Key.empty())
        throw std::runtime_error("empty key");
      const auto FullKey = Section.empty() ? Key : Section + "." + Key;
      if (!Values
               .emplace(FullKey,
                        ParseValue(std::string_view(Clean).substr(Equal + 1)))
               .second)
        throw std::runtime_error("duplicate key '" + FullKey + "'");
    } catch (const std::exception &Error) {
      throw std::runtime_error(Path.string() + ":" +
                               std::to_string(LineNumber) + ": " +
                               Error.what());
    }
  }
  return Values;
}

template <typename T>
T Get(const std::map<std::string, Value> &Values, std::string_view Key,
      std::optional<T> Default = std::nullopt) {
  const auto It = Values.find(std::string(Key));
  if (It == Values.end()) {
    if (Default)
      return *Default;
    throw std::runtime_error("missing required key '" + std::string(Key) + "'");
  }
  const auto *Result = std::get_if<T>(&It->second);
  if (!Result)
    throw std::runtime_error("wrong type for key '" + std::string(Key) + "'");
  return *Result;
}

Config LoadConfig(const fs::path &Root) {
  const auto Values = ParseToml(Root / "kelp.toml");
  static const std::vector<std::string> Known{
      "project.name",     "project.version", "project.entry",
      "build.compiler",   "build.output",    "build.optimization",
      "build.safe-level", "build.c-sources", "build.c-args",
      "package.output",   "test.sources"};
  std::set<std::string> DependencyNames;
  for (const auto &[Key, Ignored] : Values) {
    (void)Ignored;
    bool Found = false;
    for (const auto &Candidate : Known)
      Found |= Key == Candidate;
    constexpr std::string_view Prefix = "dependencies.";
    if (!Found && Key.compare(0, Prefix.size(), Prefix) == 0) {
      const auto Rest = Key.substr(Prefix.size());
      const auto Dot = Rest.find('.');
      if (Dot == std::string::npos ||
          Rest.find('.', Dot + 1) != std::string::npos)
        throw std::runtime_error("invalid dependency key '" + Key + "'");
      const auto Name = Rest.substr(0, Dot);
      const auto Field = Rest.substr(Dot + 1);
      if (!ValidName(Name) || (Field != "repository" && Field != "revision"))
        throw std::runtime_error("invalid dependency key '" + Key + "'");
      DependencyNames.insert(Name);
      Found = true;
    }
    if (!Found)
      throw std::runtime_error("unknown key '" + Key + "'");
  }
  Config Result;
  Result.Name = Get<std::string>(Values, "project.name");
  Result.Version =
      Get<std::string>(Values, "project.version", std::string("0.1.0"));
  Result.Entry =
      Get<std::string>(Values, "project.entry", std::string("src/main.kly"));
  Result.Compiler =
      Get<std::string>(Values, "build.compiler", std::string("kelyra"));
  Result.Output = Get<std::string>(Values, "build.output",
                                   std::string("build/") + Result.Name);
  Result.PackageOutput = Get<std::string>(Values, "package.output",
                                          std::string("build/") + Result.Name +
                                              "-" + Result.Version + ".tar.gz");
  const auto Optimization =
      Get<std::uint64_t>(Values, "build.optimization", std::uint64_t{0});
  const auto SafeLevel =
      Get<std::uint64_t>(Values, "build.safe-level", std::uint64_t{0});
  if (Optimization > 3)
    throw std::runtime_error("build.optimization must be between 0 and 3");
  if (SafeLevel > 255)
    throw std::runtime_error("build.safe-level must be at most 255");
  Result.Optimization = static_cast<unsigned>(Optimization);
  Result.SafeLevel = static_cast<unsigned>(SafeLevel);
  Result.CSources = Get<std::vector<std::string>>(Values, "build.c-sources",
                                                  std::vector<std::string>{});
  Result.CArguments = Get<std::vector<std::string>>(Values, "build.c-args",
                                                    std::vector<std::string>{});
  Result.TestSources = Get<std::vector<std::string>>(
      Values, "test.sources", std::vector<std::string>{});
  for (const auto &Name : DependencyNames) {
    Config::Dependency Dependency;
    Dependency.Name = Name;
    Dependency.Repository =
        Get<std::string>(Values, "dependencies." + Name + ".repository");
    Dependency.Revision = Get<std::string>(
        Values, "dependencies." + Name + ".revision", std::string{});
    if (Dependency.Repository.empty() || Dependency.Repository.front() == '-' ||
        (!Dependency.Revision.empty() && Dependency.Revision.front() == '-'))
      throw std::runtime_error(
          "invalid dependency repository or revision for '" + Name + "'");
    Result.Dependencies.push_back(std::move(Dependency));
  }
  if (!SafeRelativePath(Result.Entry) || Result.Entry.parent_path().empty())
    throw std::runtime_error("project.entry must be inside a source directory");
  if (!SafeRelativePath(Result.Output) ||
      !SafeRelativePath(Result.PackageOutput))
    throw std::runtime_error(
        "build and package outputs must be relative paths");
  for (const auto &Source : Result.CSources)
    if (!SafeRelativePath(Source))
      throw std::runtime_error("build.c-sources must contain relative paths");
  for (const auto &Source : Result.TestSources)
    if (!SafeRelativePath(Source))
      throw std::runtime_error("test.sources must contain relative paths");
  return Result;
}

fs::path FindRoot() {
  auto Current = fs::current_path();
  while (true) {
    if (fs::exists(Current / "kelp.toml"))
      return Current;
    if (Current == Current.root_path())
      throw std::runtime_error("could not find kelp.toml");
    Current = Current.parent_path();
  }
}

int Execute(const fs::path &Root, const std::vector<std::string> &Arguments) {
  if (Arguments.empty())
    throw std::runtime_error("empty command");
  const pid_t Child = fork();
  if (Child < 0)
    throw std::runtime_error("cannot create process");
  if (Child == 0) {
    if (chdir(Root.c_str()) != 0) {
      std::cerr << "kelp: cannot enter " << Root << ": " << std::strerror(errno)
                << '\n';
      _exit(126);
    }
    std::vector<char *> Argv;
    for (const auto &Argument : Arguments)
      Argv.push_back(const_cast<char *>(Argument.c_str()));
    Argv.push_back(nullptr);
    execvp(Argv.front(), Argv.data());
    std::cerr << "kelp: cannot execute " << Arguments.front() << ": "
              << std::strerror(errno) << '\n';
    _exit(errno == ENOENT ? 127 : 126);
  }
  int Status = 0;
  while (waitpid(Child, &Status, 0) < 0) {
    if (errno != EINTR)
      throw std::runtime_error("cannot wait for process");
  }
  if (WIFEXITED(Status))
    return WEXITSTATUS(Status);
  return 128 + WTERMSIG(Status);
}

struct ResolvedDependency {
  fs::path Root;
  Config Project;
};

void ResolveDependencies(const fs::path &ProjectRoot, const Config &Project,
                         std::vector<ResolvedDependency> &Result,
                         std::set<std::string> &Resolving,
                         std::set<std::string> &Resolved) {
  const auto Cache = ProjectRoot / ".kelp/dependencies";
  fs::create_directories(Cache);
  for (const auto &Dependency : Project.Dependencies) {
    if (Resolved.count(Dependency.Name))
      continue;
    if (!Resolving.insert(Dependency.Name).second)
      throw std::runtime_error("cyclic dependency involving '" +
                               Dependency.Name + "'");
    const auto DependencyRoot = Cache / Dependency.Name;
    if (!fs::exists(DependencyRoot)) {
      std::cout << "fetching " << Dependency.Name << " from "
                << Dependency.Repository << '\n';
      if (const int Status = Execute(ProjectRoot, {"git", "clone", "--quiet",
                                                   "--", Dependency.Repository,
                                                   DependencyRoot.string()}))
        throw std::runtime_error("git clone failed with status " +
                                 std::to_string(Status));
    } else if (!fs::exists(DependencyRoot / ".git")) {
      throw std::runtime_error("dependency cache is not a Git repository: " +
                               DependencyRoot.string());
    } else if (const int Status =
                   Execute(DependencyRoot, {"git", "remote", "set-url",
                                            "origin", Dependency.Repository})) {
      throw std::runtime_error("cannot update dependency remote, status " +
                               std::to_string(Status));
    }
    if (!Dependency.Revision.empty()) {
      if (const int Status =
              Execute(DependencyRoot, {"git", "fetch", "--quiet", "origin",
                                       Dependency.Revision}))
        throw std::runtime_error("git fetch failed with status " +
                                 std::to_string(Status));
      if (const int Status =
              Execute(DependencyRoot,
                      {"git", "checkout", "--quiet", "--detach", "FETCH_HEAD"}))
        throw std::runtime_error("git checkout failed with status " +
                                 std::to_string(Status));
    }
    auto DependencyProject = LoadConfig(DependencyRoot);
    ResolveDependencies(ProjectRoot, DependencyProject, Result, Resolving,
                        Resolved);
    Result.push_back({DependencyRoot, std::move(DependencyProject)});
    Resolving.erase(Dependency.Name);
    Resolved.insert(Dependency.Name);
  }
}

std::vector<ResolvedDependency> ResolveDependencies(const fs::path &Root,
                                                    const Config &Project) {
  std::vector<ResolvedDependency> Result;
  std::set<std::string> Resolving;
  std::set<std::string> Resolved;
  ResolveDependencies(Root, Project, Result, Resolving, Resolved);
  return Result;
}

Config Prepare(const fs::path &Root, const Config &Project) {
  Config Result = Project;
  // Sources are compiled where they live. Instead of copying the project and
  // dependency sources into a staging tree, every source directory is passed
  // to the compiler as a module search path.
  Result.ModulePaths.push_back(fs::absolute(Root / Project.Entry.parent_path())
                                   .lexically_normal()
                                   .string());
  // Remove the staging tree created by older Kelp versions so no stale copies
  // remain next to the originals.
  std::error_code Error;
  fs::remove_all(Root / ".kelp/stage", Error);
  if (Project.Dependencies.empty())
    return Result;
  for (const auto &Dependency : ResolveDependencies(Root, Project)) {
    const auto DependencySourceRoot = Dependency.Project.Entry.parent_path();
    Result.ModulePaths.push_back(
        fs::absolute(Dependency.Root / DependencySourceRoot)
            .lexically_normal()
            .string());
    for (const auto &Source : Dependency.Project.CSources)
      Result.CSources.push_back(
          fs::absolute(Dependency.Root / Source).string());
    Result.CArguments.insert(Result.CArguments.end(),
                             Dependency.Project.CArguments.begin(),
                             Dependency.Project.CArguments.end());
  }
  return Result;
}

std::vector<std::string> CompilerCommand(const Config &Config,
                                         std::string Action) {
  std::vector<std::string> Result{Config.Compiler, std::move(Action)};
  for (const auto &Path : Config.ModulePaths)
    Result.push_back("--module-path=" + Path);
  if (Result[1] == "--emit-exe") {
    Result.push_back("--progress");
    Result.push_back("-O" + std::to_string(Config.Optimization));
    Result.push_back("--safe-level=" + std::to_string(Config.SafeLevel));
    for (const auto &Source : Config.CSources)
      Result.push_back("--c-source=" + Source);
    for (const auto &Argument : Config.CArguments)
      Result.push_back("--c-arg=" + Argument);
    Result.push_back("-o");
    Result.push_back(Config.Output.string());
  }
  Result.push_back(Config.Entry.string());
  return Result;
}

int Check(const fs::path &Root, const Config &Config) {
  const auto Prepared = Prepare(Root, Config);
  return Execute(Root, CompilerCommand(Prepared, "--check"));
}

int Build(const fs::path &Root, const Config &Config) {
  std::cerr << "[1/3] Preparing " << Config.Name << '\n';
  std::error_code Error;
  fs::create_directories(Root / Config.Output.parent_path(), Error);
  if (Error)
    throw std::runtime_error("cannot create build directory: " +
                             Error.message());
  const auto Prepared = Prepare(Root, Config);
  std::cerr << "[2/3] Building " << Prepared.Entry.string() << " -> "
            << Config.Output.string() << '\n';
  const int Status = Execute(Root, CompilerCommand(Prepared, "--emit-exe"));
  if (Status == 0)
    std::cerr << "[3/3] Finished " << Config.Output.string() << '\n';
  else
    std::cerr << "Build failed (exit " << Status << ")\n";
  return Status;
}

int Package(const fs::path &Root, const Config &Config) {
  if (const int Status = Build(Root, Config))
    return Status;
  std::error_code Error;
  fs::create_directories(Root / Config.PackageOutput.parent_path(), Error);
  if (Error)
    throw std::runtime_error("cannot create package directory: " +
                             Error.message());
  std::vector<std::string> Arguments{"tar", "-czf",
                                     Config.PackageOutput.string(), "kelp.toml",
                                     Config.Entry.parent_path().string()};
  if (fs::exists(Root / "README.md"))
    Arguments.push_back("README.md");
  return Execute(Root, Arguments);
}

bool ValidName(std::string_view Name) {
  if (Name.empty() || !std::isalpha(static_cast<unsigned char>(Name.front())))
    return false;
  for (const char Character : Name)
    if (!std::isalnum(static_cast<unsigned char>(Character)) &&
        Character != '-' && Character != '_')
      return false;
  return true;
}

void CreateProject(const fs::path &Root, std::string Name) {
  if (!ValidName(Name))
    throw std::runtime_error(
        "project name must use letters, digits, '-' or '_'");
  if (fs::exists(Root / "kelp.toml"))
    throw std::runtime_error("kelp.toml already exists");
  if (fs::exists(Root / "src/main.kly"))
    throw std::runtime_error("src/main.kly already exists");
  fs::create_directories(Root / "src");
  std::ofstream Toml(Root / "kelp.toml");
  if (!Toml)
    throw std::runtime_error("cannot create kelp.toml");
  Toml << "[project]\n"
          "name = \""
       << Name
       << "\"\n"
          "version = \"0.1.0\"\n"
          "entry = \"src/main.kly\"\n\n"
          "[build]\n"
          "compiler = \"kelyra\"\n"
          "output = \"build/"
       << Name
       << "\"\n"
          "optimization = 0\n"
          "safe-level = 0\n"
          "c-sources = []\n"
          "c-args = []\n\n"
          "[package]\n"
          "output = \"build/"
       << Name
       << "-0.1.0.tar.gz\"\n\n"
          "[test]\n"
          "sources = []\n\n"
          "# [dependencies.kstd]\n"
          "# repository = \"git@github.com:z8z6/kstd.git\"\n"
          "# revision = \"main\"\n";
  std::ofstream Main(Root / "src/main.kly");
  if (!Main)
    throw std::runtime_error("cannot create src/main.kly");
  Main << "pub fn main() -> i32 {\n  return 0;\n}\n";
  if (!fs::exists(Root / ".gitignore")) {
    std::ofstream Ignore(Root / ".gitignore");
    Ignore << "/build/\n/.kelp/\n";
  }
  std::cout << "created " << Name << " in " << Root << '\n';
}

void Help() {
  std::cout << "Kelp - Kelyra project manager\n\n"
               "usage: kelp <command> [arguments]\n\n"
               "commands:\n"
               "  new <name>    Create a project in ./<name>\n"
               "  init [name]   Create a project in the current directory\n"
               "  check         Type-check the project\n"
               "  build [--debug] Build the executable (--debug uses -O0)\n"
               "  output        Print the absolute executable path\n"
               "  run [-- ...]  Build and run the project\n"
               "  test          Check configured test sources\n"
               "  package       Build and create a source archive\n"
               "  help          Show this help\n";
}
} // namespace

int main(int Argc, char **Argv) {
  try {
    if (Argc == 2 && std::string_view(Argv[1]) == "--version") {
      std::cout << "kelp " << KELP_VERSION << '\n';
      return 0;
    }
    if (Argc < 2 || std::string_view(Argv[1]) == "help" ||
        std::string_view(Argv[1]) == "-h" ||
        std::string_view(Argv[1]) == "--help") {
      Help();
      return Argc < 2 ? 2 : 0;
    }
    const std::string Command = Argv[1];
    if (Command == "new") {
      if (Argc != 3)
        throw std::runtime_error("usage: kelp new <name>");
      CreateProject(fs::current_path() / Argv[2], Argv[2]);
      return 0;
    }
    if (Command == "init") {
      if (Argc > 3)
        throw std::runtime_error("usage: kelp init [name]");
      auto Name = Argc == 3 ? std::string(Argv[2])
                            : fs::current_path().filename().string();
      CreateProject(fs::current_path(), std::move(Name));
      return 0;
    }

    const auto Root = FindRoot();
    const auto Project = LoadConfig(Root);
    if (Command == "check") {
      if (Argc != 2)
        throw std::runtime_error("usage: kelp check");
      return Check(Root, Project);
    }
    if (Command == "build") {
      if (Argc != 2 && !(Argc == 3 && std::string_view(Argv[2]) == "--debug"))
        throw std::runtime_error("usage: kelp build [--debug]");
      auto BuildProject = Project;
      if (Argc == 3)
        BuildProject.Optimization = 0;
      return Build(Root, BuildProject);
    }
    if (Command == "output") {
      if (Argc != 2)
        throw std::runtime_error("usage: kelp output");
      std::cout << (Root / Project.Output).string() << '\n';
      return 0;
    }
    if (Command == "run") {
      if (const int Status = Build(Root, Project))
        return Status;
      std::vector<std::string> Arguments{(Root / Project.Output).string()};
      int Start = 2;
      if (Start < Argc && std::string_view(Argv[Start]) == "--")
        ++Start;
      for (int I = Start; I < Argc; ++I)
        Arguments.emplace_back(Argv[I]);
      return Execute(Root, Arguments);
    }
    if (Command == "test") {
      if (Argc != 2)
        throw std::runtime_error("usage: kelp test");
      const auto Prepared = Prepare(Root, Project);
      if (Prepared.TestSources.empty())
        return Execute(Root, CompilerCommand(Prepared, "--check"));
      for (const auto &Source : Prepared.TestSources) {
        auto Arguments = CompilerCommand(Prepared, "--check");
        Arguments.back() = Source;
        if (const int Status = Execute(Root, Arguments))
          return Status;
      }
      return 0;
    }
    if (Command == "package") {
      if (Argc != 2)
        throw std::runtime_error("usage: kelp package");
      return Package(Root, Project);
    }
    throw std::runtime_error("unknown command '" + Command + "'");
  } catch (const std::exception &Error) {
    std::cerr << "kelp: " << Error.what() << '\n';
    return 2;
  }
}
