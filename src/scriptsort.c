/**
 * scriptsort.c
 *
 * Sorts and outputs shell script files from a directory according to an
 * ordering convention, with subcommands for listing, bundling, init-script
 * generation, and in-place editing.
 *
 * Ordering rules:
 *   1. ordered.(0-49).*   lower-numbered files, ascending
 *   2. (unordered files)  alphabetically
 *   3. ordered.(50+).*    upper-numbered files, ascending
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define SCRIPTSORT_VERSION   "1.0.0"

#define MAX_FILES            1000
#define MAX_FILENAME         256
#define INITIAL_BUFFER_SIZE  4096

/* SGR / CSI color codes — description strings carry no SGR, renderer owns it */
#define SGR_BOLD   "\033[1m"
#define SGR_DIM    "\033[2m"
#define SGR_RED    "\033[31m"
#define SGR_GREEN  "\033[32m"
#define SGR_YELLOW "\033[33m"
#define SGR_CYAN   "\033[36m"
#define SGR_RESET  "\033[22;39m"

/* Sub-directory names used by edit and --scripts-dir */
#define SUB_SHARED "shared"
#define SUB_BASH   "bash"
#define SUB_ZSH    "zsh"

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

/* Edit subcommand operations */
typedef enum { CMD_NONE, CMD_WRITE, CMD_APPEND, CMD_REMOVE } Command;

/**
 * Flag definition — carries both matching data and help display text.
 * Description must be plain text; the renderer applies all SGR codes.
 */
typedef struct {
  const char *short_name;  /* "-h"          or NULL for long-only flags  */
  const char *long_name;   /* "--help"                                    */
  const char *arg_hint;    /* "<directory>"  or NULL for boolean switches */
  const char *description; /* plain English, no escape codes              */
} FlagDef;

/**
 * Subcommand dispatch entry.
 * Each subcommand owns its flag array and run function.
 */
typedef struct {
  const char *name;
  const char *description;
  const char *usage;
  const FlagDef *flags;    /* NULL-terminated FlagDef array */
  int (*run)(int argc, char **argv);
} Subcommand;

typedef char          *String;
typedef unsigned char  Boolean;
static const Boolean   Truth     = 1;
static const Boolean   Falsehood = 0;

/* File entry produced by directory scanning */
typedef struct {
  char         name[MAX_FILENAME];
  int          order_num;    /* -1 for unordered files */
  unsigned int bytesize;
} FileEntry;

/* Result of load_sorted_dir() */
typedef struct {
  FileEntry lower_files[MAX_FILES];
  FileEntry upper_files[MAX_FILES];
  FileEntry unordered_files[MAX_FILES];
  int lower_count;
  int upper_count;
  int unordered_count;
  int total_bytesize;   /* sum of (name_len + 2) across all entries */
} SortedDir;

/* -------------------------------------------------------------------------
 * Flag definitions — one NULL-terminated array per subcommand.
 * Add a new flag here; the generic renderer handles formatting.
 * ---------------------------------------------------------------------- */

static const FlagDef LIST_FLAGS[] = {
  { "-h", "--help",   NULL,  "show this help"                                },
  { NULL, "--cutoff", "<n>", "change the ordered file cutoff (default: 50)"  },
  { NULL, NULL, NULL, NULL }
};

static const FlagDef BUNDLE_FLAGS[] = {
  { "-h", "--help",        NULL,        "show this help"                                      },
  { NULL, "--scripts-dir", "<base-dir>","bundle shared/ then the detected shell sub-directory"},
  { NULL, "--debug",       NULL,        "emit timing variables around the bundle"              },
  { NULL, "--cutoff",      "<n>",       "change the ordered file cutoff (default: 50)"         },
  { NULL, NULL, NULL, NULL }
};

static const FlagDef INIT_FLAGS[] = {
  { "-h", "--help",   NULL,  "show this help"                                    },
  { NULL, "--debug",  NULL,  "emit per-file timing in the generated wrapper"     },
  { NULL, "--cutoff", "<n>", "change the ordered file cutoff (default: 50)"      },
  { NULL, NULL, NULL, NULL }
};

static const FlagDef EDIT_FLAGS[] = {
  { "-h", "--help",   NULL,  "show this help"                             },
  { NULL, "--shared", NULL,  "operate in the shared/ directory (default)" },
  { NULL, "--bash",   NULL,  "operate in the bash/ directory"             },
  { NULL, "--zsh",    NULL,  "operate in the zsh/ directory"              },
  { NULL, NULL, NULL, NULL }
};

/* -------------------------------------------------------------------------
 * Subcommand run-function prototypes (defined later in file)
 * ---------------------------------------------------------------------- */

static int list_main(int argc, char **argv);
static int bundle_main(int argc, char **argv);
static int init_main(int argc, char **argv);
static int edit_main(int argc, char **argv);

/* -------------------------------------------------------------------------
 * Subcommand dispatch table
 * Add a new subcommand here alongside its flag array and run function.
 * ---------------------------------------------------------------------- */

static const Subcommand SUBCOMMANDS[] = {
  {
    "list",
    "print filenames in sorted order",
    "list <directory> [options]",
    LIST_FLAGS,
    list_main
  },
  {
    "bundle",
    "concatenate script contents into a single output",
    "bundle <directory> [options]\n"
    "       bundle --scripts-dir <base-dir> [options]",
    BUNDLE_FLAGS,
    bundle_main
  },
  {
    "init",
    "emit a self-contained shell sourcing wrapper",
    "init <directory> [options]",
    INIT_FLAGS,
    init_main
  },
  {
    "edit",
    "write, append, or remove script files",
    "edit [--shared|--bash|--zsh] <command> <file> [text]\n"
    "  commands:  write [-f|--force] [-q|--quiet] <file> [text]\n"
    "             append [-q|--quiet] <file> [text]\n"
    "             remove <file>",
    EDIT_FLAGS,
    edit_main
  },
  { NULL, NULL, NULL, NULL, NULL }
};

/* -------------------------------------------------------------------------
 * Static utility prototypes
 * ---------------------------------------------------------------------- */

static const Subcommand *find_subcommand(const char *name);
static void  print_top_level_usage(const char *progname);
static void  print_subcommand_help(const char *progname, const Subcommand *cmd);
static int   load_sorted_dir(const char *path, unsigned int cutoff, SortedDir *out);
static int   bundle_append_dir(const char *dir_path, SortedDir *sd,
               char **buffer, size_t *capacity, size_t *size, int *line_offset);

static const char *find_last_path_separator(const char *path);
static int         extract_order_number(const char *filename);
static const char *extract_suffix(const char *filename);
static int         compare_ordered_files(const void *a, const void *b);
static int    compare_unordered(const void *a, const void *b);
static char  *read_file_contents(const char *directory, const char *filename, size_t *size);
static char  *ensure_buffer_capacity(char *buffer, size_t *capacity, size_t needed);
static size_t count_lines(const char *content, size_t size);

/* Edit-subcommand helpers */
static int   FlagMatches(FlagDef flag, const char *argument);
static int   file_exists(const char *path);
static char *build_path(const char *sub_dir, const char *filename);
static char *read_stdin_to_buffer(void);

/* =========================================================================
 * main — global flag handling and subcommand dispatch
 * ====================================================================== */

int main(int argc, char **argv) {
  const char *progname = argv[0];
  const char *sep      = find_last_path_separator(progname);
  if (sep) progname = sep + 1;

  if (argc < 2) {
    print_top_level_usage(progname);
    return EXIT_FAILURE;
  }

  if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
    printf("scriptsort v%s\n", SCRIPTSORT_VERSION);
    return EXIT_SUCCESS;
  }

  const Subcommand *cmd = find_subcommand(argv[1]);
  if (!cmd) {
    fprintf(stderr, SGR_RED "Unknown subcommand: %s\n\n" SGR_RESET, argv[1]);
    print_top_level_usage(progname);
    return EXIT_FAILURE;
  }

  /* argv[0] inside each run function is the subcommand name */
  return cmd->run(argc - 1, argv + 1);
}

/* =========================================================================
 * Help output
 * ====================================================================== */

static const Subcommand *find_subcommand(const char *name) {
  for (int i = 0; SUBCOMMANDS[i].name; i++)
    if (strcmp(SUBCOMMANDS[i].name, name) == 0)
      return &SUBCOMMANDS[i];
  return NULL;
}

static void print_top_level_usage(const char *progname) {
  fprintf(stderr,
    SGR_BOLD "%s" SGR_RESET " v%s\n\n"
    SGR_BOLD "Usage:" SGR_RESET " %s <subcommand> [options]\n\n"
    SGR_BOLD "Subcommands:\n" SGR_RESET,
    progname, SCRIPTSORT_VERSION, progname
  );

  for (int i = 0; SUBCOMMANDS[i].name; i++) {
    fprintf(stderr, "  " SGR_CYAN "%-8s" SGR_RESET "  %s\n",
      SUBCOMMANDS[i].name, SUBCOMMANDS[i].description);
  }

  fprintf(stderr,
    "\n"
    SGR_BOLD "Global flags:\n" SGR_RESET
    "  " SGR_CYAN "-v, --version" SGR_RESET "  print version and exit\n\n"
    "Run " SGR_BOLD "%s <subcommand> --help" SGR_RESET
    " for subcommand-specific options.\n\n",
    progname
  );
}

/**
 * Generic subcommand help renderer.
 * Iterates cmd->flags and formats each row with consistent SGR styling.
 * All description strings must be plain text — SGR lives here, not in FlagDef.
 */
static void print_subcommand_help(const char *progname, const Subcommand *cmd) {
  fprintf(stderr,
    SGR_BOLD "%s %s\n" SGR_RESET
    SGR_DIM  "%s" SGR_RESET "\n\n"
    SGR_BOLD "Options:\n" SGR_RESET,
    progname, cmd->usage, cmd->description
  );

  for (int i = 0; cmd->flags[i].long_name; i++) {
    const FlagDef *f = &cmd->flags[i];
    char names[48]   = {0};

    if (f->short_name)
      snprintf(names, sizeof(names), "%s, %s", f->short_name, f->long_name);
    else
      snprintf(names, sizeof(names), "    %s", f->long_name);

    /* Align: names col = 22, arg_hint col = 14, then description */
    fprintf(stderr, "  " SGR_CYAN "%-22s" SGR_RESET " " SGR_YELLOW "%-14s" SGR_RESET " %s\n",
      names,
      f->arg_hint ? f->arg_hint : "",
      f->description);
  }

  fprintf(stderr, "\n");
}

/* =========================================================================
 * Shared directory loading
 * ====================================================================== */

/**
 * Opens path, reads all non-skipped entries, categorises them into
 * lower/unordered/upper buckets, and sorts each bucket.
 * Returns 0 on success, -1 on failure (error printed to stderr).
 */
static int load_sorted_dir(const char *path, unsigned int cutoff, SortedDir *out) {
  memset(out, 0, sizeof(*out));

  DIR *dir = opendir(path);
  if (!dir) {
    fprintf(stderr, "Error opening directory '%s': %s\n", path, strerror(errno));
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (strncmp(entry->d_name, "skip.", 5) == 0)
      continue;

    int order_num = extract_order_number(entry->d_name);
    FileEntry fe  = {0};
    strncpy(fe.name, entry->d_name, MAX_FILENAME - 1);
    fe.order_num = order_num;
    fe.bytesize  = (unsigned int)strlen(fe.name);
    out->total_bytesize += (int)fe.bytesize + 2;

    if (order_num >= 0 && (unsigned int)order_num < cutoff) {
      if (out->lower_count < MAX_FILES)
        out->lower_files[out->lower_count++] = fe;
    } else if (order_num >= 0) {
      if (out->upper_count < MAX_FILES)
        out->upper_files[out->upper_count++] = fe;
    } else {
      if (out->unordered_count < MAX_FILES)
        out->unordered_files[out->unordered_count++] = fe;
    }
  }
  closedir(dir);

  qsort(out->lower_files,     out->lower_count,     sizeof(FileEntry), compare_ordered_files);
  qsort(out->upper_files,     out->upper_count,     sizeof(FileEntry), compare_ordered_files);
  qsort(out->unordered_files, out->unordered_count, sizeof(FileEntry), compare_unordered);

  return 0;
}

/**
 * Appends every file in sd (lower → unordered → upper) to an existing
 * bundle buffer, injecting section headers and updating line_offset so
 * that the _SCRIPTSORT_OFFSET values reflect real bundle line numbers.
 */
static int bundle_append_dir(
  const char *dir_path, SortedDir *sd,
  char **buffer, size_t *capacity, size_t *size,
  int *line_offset
) {
  char   header[MAX_FILENAME * 2 + 128];
  size_t header_len;
  size_t file_size;
  char  *file_contents;

  FileEntry *groups[3] = { sd->lower_files, sd->unordered_files, sd->upper_files };
  int        counts[3] = { sd->lower_count, sd->unordered_count, sd->upper_count };

  for (int g = 0; g < 3; g++) {
    for (int i = 0; i < counts[g]; i++) {
      file_contents = read_file_contents(dir_path, groups[g][i].name, &file_size);
      if (!file_contents) continue;

      /* Header is 4 lines: blank + comment + _FILE + _OFFSET */
      size_t file_lines = count_lines(file_contents, file_size);
      int    file_start = *line_offset + 5;
      int    file_end   = file_start + (file_lines > 0 ? (int)file_lines - 1 : 0);

      header_len = (size_t)snprintf(header, sizeof(header),
        "\n# --- %s (lines %d-%d) ---\n_SCRIPTSORT_FILE='%s'\n_SCRIPTSORT_OFFSET=%d\n",
        groups[g][i].name, file_start, file_end,
        groups[g][i].name, file_start);
      *line_offset = file_end + 1;

      *buffer = ensure_buffer_capacity(*buffer, capacity, *size + header_len + file_size + 2);
      if (!*buffer) { free(file_contents); return -1; }

      strcat(*buffer + *size, header);
      *size += header_len;
      strcat(*buffer + *size, file_contents);
      *size += file_size;
      (*buffer)[(*size)++] = '\n';
      (*buffer)[*size]      = '\0';
      free(file_contents);
    }
  }
  return 0;
}

/* =========================================================================
 * list subcommand
 * ====================================================================== */

static int list_main(int argc, char **argv) {
  const char  *directory    = NULL;
  unsigned int cutoff_count = 50;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_subcommand_help("scriptsort", find_subcommand("list"));
      return EXIT_SUCCESS;
    } else if (strcmp(argv[i], "--cutoff") == 0 && i + 1 < argc) {
      int n = atoi(argv[++i]);
      if (n <= 0) {
        fprintf(stderr, SGR_RED "--cutoff requires a number greater than 0\n" SGR_RESET);
        return EXIT_FAILURE;
      }
      cutoff_count = (unsigned int)n;
    } else if (argv[i][0] != '-' && !directory) {
      directory = argv[i];
    } else {
      fprintf(stderr, SGR_RED "Unknown argument: %s\n" SGR_RESET, argv[i]);
      return EXIT_FAILURE;
    }
  }

  if (!directory) {
    print_subcommand_help("scriptsort", find_subcommand("list"));
    return EXIT_FAILURE;
  }

  SortedDir sd;
  if (load_sorted_dir(directory, cutoff_count, &sd) != 0)
    return EXIT_FAILURE;

  String buffer = calloc(1, (size_t)sd.total_bytesize + 1);
  if (!buffer) {
    fprintf(stderr, "Cannot allocate buffer\n");
    return EXIT_FAILURE;
  }

  size_t cur = 0;
  for (int i = 0; i < sd.lower_count;     i++) cur += (size_t)sprintf(buffer + cur, "%s\n", sd.lower_files[i].name);
  for (int i = 0; i < sd.unordered_count; i++) cur += (size_t)sprintf(buffer + cur, "%s\n", sd.unordered_files[i].name);
  for (int i = 0; i < sd.upper_count;     i++) cur += (size_t)sprintf(buffer + cur, "%s\n", sd.upper_files[i].name);

  printf("%s", buffer);
  free(buffer);
  return EXIT_SUCCESS;
}

/* =========================================================================
 * bundle subcommand
 * ====================================================================== */

static int bundle_main(int argc, char **argv) {
  const char  *directory    = NULL;
  const char  *scripts_dir  = NULL;
  Boolean      debugtext    = Falsehood;
  unsigned int cutoff_count = 50;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_subcommand_help("scriptsort", find_subcommand("bundle"));
      return EXIT_SUCCESS;
    } else if (strcmp(argv[i], "--scripts-dir") == 0 && i + 1 < argc) {
      scripts_dir = argv[++i];
    } else if (strcmp(argv[i], "--debug") == 0) {
      debugtext = Truth;
    } else if (strcmp(argv[i], "--cutoff") == 0 && i + 1 < argc) {
      int n = atoi(argv[++i]);
      if (n <= 0) {
        fprintf(stderr, SGR_RED "--cutoff requires a number greater than 0\n" SGR_RESET);
        return EXIT_FAILURE;
      }
      cutoff_count = (unsigned int)n;
    } else if (argv[i][0] != '-' && !directory && !scripts_dir) {
      directory = argv[i];
    } else {
      fprintf(stderr, SGR_RED "Unknown argument: %s\n" SGR_RESET, argv[i]);
      return EXIT_FAILURE;
    }
  }

  if (scripts_dir && directory) {
    fprintf(stderr, SGR_RED "--scripts-dir and <directory> are mutually exclusive\n" SGR_RESET);
    return EXIT_FAILURE;
  }
  if (!scripts_dir && !directory) {
    print_subcommand_help("scriptsort", find_subcommand("bundle"));
    return EXIT_FAILURE;
  }

  size_t buffer_capacity = INITIAL_BUFFER_SIZE;
  size_t current_size    = 0;
  /* Preamble emits 4 code lines + 1 blank = 5 lines; debug start_time adds 1 more */
  int    line_offset     = (debugtext ? 1 : 0) + 5;

  char *buffer = malloc(buffer_capacity);
  if (!buffer) {
    fprintf(stderr, "Failed to allocate initial buffer\n");
    return EXIT_FAILURE;
  }
  buffer[0] = '\0';

  if (scripts_dir) {
    /*
     * Detect the shell that invoked scriptsort.
     *
     * ZSH_VERSION / BASH_VERSION are checked first — they are definitive when
     * exported, but many shells do not export them by default.
     *
     * $SHELL is the fallback: it is always exported (set by the login process)
     * and correct for the common case where login shell == current shell. It
     * would misidentify the shell only if the user launched a different shell
     * interactively, which is rare enough to be acceptable.
     */
    const char *shell_subdir = NULL;
    if (getenv("ZSH_VERSION")) {
      shell_subdir = SUB_ZSH;
    } else if (getenv("BASH_VERSION")) {
      shell_subdir = SUB_BASH;
    } else {
      const char *shell = getenv("SHELL");
      if (shell) {
        const char *name = find_last_path_separator(shell);
        name = name ? name + 1 : shell;
        if      (strcmp(name, "zsh")  == 0) shell_subdir = SUB_ZSH;
        else if (strcmp(name, "bash") == 0) shell_subdir = SUB_BASH;
      }
    }

    char      path[PATH_MAX];
    struct stat st;

    /* shared/ — always first */
    snprintf(path, sizeof(path), "%s/" SUB_SHARED, scripts_dir);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      SortedDir sd;
      if (load_sorted_dir(path, cutoff_count, &sd) == 0) {
        if (bundle_append_dir(path, &sd, &buffer, &buffer_capacity, &current_size, &line_offset) != 0) {
          free(buffer); return EXIT_FAILURE;
        }
      }
    }

    /* Shell-specific sub-directory — only when shell is detected */
    if (shell_subdir) {
      snprintf(path, sizeof(path), "%s/%s", scripts_dir, shell_subdir);
      if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        SortedDir sd;
        if (load_sorted_dir(path, cutoff_count, &sd) == 0) {
          if (bundle_append_dir(path, &sd, &buffer, &buffer_capacity, &current_size, &line_offset) != 0) {
            free(buffer); return EXIT_FAILURE;
          }
        }
      }
    }

  } else {
    SortedDir sd;
    if (load_sorted_dir(directory, cutoff_count, &sd) != 0) {
      free(buffer); return EXIT_FAILURE;
    }
    if (bundle_append_dir(directory, &sd, &buffer, &buffer_capacity, &current_size, &line_offset) != 0) {
      free(buffer); return EXIT_FAILURE;
    }
  }

  if (debugtext) {
    printf("local start_time=%s\n",
      "$(command 2>&1 >/dev/null -v ms && ms || printf '0')");
  }

  printf(
    "_SCRIPTSORT_OLD_TRAP=$(trap -p ERR)\n"
    "_SCRIPTSORT_FILE=''\n"
    "_SCRIPTSORT_OFFSET=0\n"
    "trap 'printf \"scriptsort: error sourcing \\\"${_SCRIPTSORT_FILE}\\\" "
      "(bundle line ${_SCRIPTSORT_OFFSET})\\n\" >&2' ERR\n"
    "\n"
  );

  printf("%s\n", buffer);

  printf(
    "\ntrap - ERR\n"
    "eval \"$_SCRIPTSORT_OLD_TRAP\"\n"
    "unset _SCRIPTSORT_OLD_TRAP _SCRIPTSORT_FILE _SCRIPTSORT_OFFSET\n"
  );

  if (debugtext) {
    printf("local end_time=%s\n",
      "$(command 2>&1 >/dev/null -v ms && ms || printf '0')");
    printf("export SCRIPTSORT_ELAPSED=$(($end_time - $start_time))\n");
  }

  free(buffer);
  return EXIT_SUCCESS;
}

/* =========================================================================
 * init subcommand
 * ====================================================================== */

static int init_main(int argc, char **argv) {
  const char  *directory    = NULL;
  Boolean      debugtext    = Falsehood;
  unsigned int cutoff_count = 50;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_subcommand_help("scriptsort", find_subcommand("init"));
      return EXIT_SUCCESS;
    } else if (strcmp(argv[i], "--debug") == 0) {
      debugtext = Truth;
    } else if (strcmp(argv[i], "--cutoff") == 0 && i + 1 < argc) {
      int n = atoi(argv[++i]);
      if (n <= 0) {
        fprintf(stderr, SGR_RED "--cutoff requires a number greater than 0\n" SGR_RESET);
        return EXIT_FAILURE;
      }
      cutoff_count = (unsigned int)n;
    } else if (argv[i][0] != '-' && !directory) {
      directory = argv[i];
    } else {
      fprintf(stderr, SGR_RED "Unknown argument: %s\n" SGR_RESET, argv[i]);
      return EXIT_FAILURE;
    }
  }

  if (!directory) {
    print_subcommand_help("scriptsort", find_subcommand("init"));
    return EXIT_FAILURE;
  }

  SortedDir sd;
  if (load_sorted_dir(directory, cutoff_count, &sd) != 0)
    return EXIT_FAILURE;

  String buffer = calloc(1, (size_t)sd.total_bytesize + 1);
  if (!buffer) {
    fprintf(stderr, "Cannot allocate buffer\n");
    return EXIT_FAILURE;
  }

  size_t cur = 0;
  for (int i = 0; i < sd.lower_count;     i++) cur += (size_t)sprintf(buffer + cur, "%s ", sd.lower_files[i].name);
  for (int i = 0; i < sd.unordered_count; i++) cur += (size_t)sprintf(buffer + cur, "%s ", sd.unordered_files[i].name);
  for (int i = 0; i < sd.upper_count;     i++) cur += (size_t)sprintf(buffer + cur, "%s ", sd.upper_files[i].name);
  (void)cur;

  const char *debugStart = debugtext
    ? "    printf \"Sourcing \\\"${scriptpath}\\\"...\"\n" : "\n";
  const char *debugEnd   = debugtext
    ? "    printf \"done\\n\"\n" : "";
  const char *timer = "$(command 2>&1 >/dev/null -v ms && ms || printf '0')";

  const char script[] = (
    "pjoin() {\n"
    "  local -a parts\n"
    "\n"
    "  if [[ \"${#}\" -lt 1 ]]; then\n"
    "    printf \"\\x1b[1;35mpjoin\\x1b[22;39m <path> <part> ...\\n\\n\"\n"
    "    printf \"Example:\\n\"\n"
    "    printf \"  pjoin \\$HOME .zshrc\\n\"\n"
    "    printf \"  \\x1b[3m/Users/${USER}/.zshrc\\x1b[33m\\n\"\n"
    "    return 0\n"
    "  fi\n"
    "\n"
    "  for part in \"${@}\"; do\n"
    "    parts+=( \"${part}\" \"/\" )\n"
    "  done\n"
    "\n"
    "  printf \"$(realpath $(printf \"${parts// /}\"))\"\n"
    "}\n"
    "\n"
    "includeScripts() {\n"
    "  local -a scripts\n"
    "  local -a timings\n"
    "  local directory=\"${1:-${HOME}/.zsh.scripts}\"\n"
    "  local scriptpath=\"\"\n"
    "  local timer\n"
    "  local now\n"
    "  local elapsed\n"
    "\n"
    "  scripts=( %s )\n"
    "  for script in \"${scripts[@]}\"; do\n"
    "    timer=%s\n"
    "    scriptpath=$(pjoin \"${directory}\" \"${script}\")\n"
    "%s"
    "    source \"${scriptpath}\"\n"
    "    if [ $timer ]; then\n"
    "      now=%s\n"
    "      elapsed=$(($now-$timer))\n"
    "\n"
    "      timings+=( \"${elapsed}ms:${scriptpath}\" )\n"
    "    fi\n"
    "%s"
    "  done\n"
    "}\n\n"
    "includeScripts \"%s\"\n"
    "unset -f includeScripts\n"
  );

  printf(script, buffer, timer, debugStart, timer, debugEnd, directory);
  free(buffer);
  return EXIT_SUCCESS;
}

/* =========================================================================
 * edit subcommand
 * ====================================================================== */

static int edit_main(int argc, char **argv) {
  const char *sub_dir       = SUB_SHARED;
  const char *filename      = NULL;
  char       *input_content = NULL;
  char       *full_path     = NULL;
  Command     cmd           = CMD_NONE;
  int         force = 0, quiet = 0, alloc_content = 0;
  FILE       *fp;

  FlagDef f_bash   = { NULL, "--bash",   NULL, NULL };
  FlagDef f_zsh    = { NULL, "--zsh",    NULL, NULL };
  FlagDef f_shared = { NULL, "--shared", NULL, NULL };
  FlagDef f_force  = { "-f", "--force",  NULL, NULL };
  FlagDef f_quiet  = { "-q", "--quiet",  NULL, NULL };
  FlagDef f_help   = { "-h", "--help",   NULL, NULL };

  FlagDef cmd_write  = { NULL, "write",  NULL, NULL };
  FlagDef cmd_append = { NULL, "append", NULL, NULL };
  FlagDef cmd_remove = { NULL, "remove", NULL, NULL };

  if (argc < 2) {
    print_subcommand_help("scriptsort", find_subcommand("edit"));
    return EXIT_FAILURE;
  }

  int i = 1;

  /* 1. Directory selector flags */
  for (; i < argc; i++) {
    if      (FlagMatches(f_help,   argv[i])) { print_subcommand_help("scriptsort", find_subcommand("edit")); return EXIT_SUCCESS; }
    else if (FlagMatches(f_bash,   argv[i])) sub_dir = SUB_BASH;
    else if (FlagMatches(f_zsh,    argv[i])) sub_dir = SUB_ZSH;
    else if (FlagMatches(f_shared, argv[i])) sub_dir = SUB_SHARED;
    else break;
  }

  /* 2. Operation */
  if (i >= argc) {
    print_subcommand_help("scriptsort", find_subcommand("edit"));
    return EXIT_FAILURE;
  }

  if      (FlagMatches(cmd_write,  argv[i])) cmd = CMD_WRITE;
  else if (FlagMatches(cmd_append, argv[i])) cmd = CMD_APPEND;
  else if (FlagMatches(cmd_remove, argv[i])) cmd = CMD_REMOVE;
  else {
    fprintf(stderr, SGR_RED "Unknown edit command: %s\n" SGR_RESET, argv[i]);
    return EXIT_FAILURE;
  }
  i++;

  /* 3. Operation flags and arguments */
  for (; i < argc; i++) {
    if      (FlagMatches(f_force, argv[i])) force = 1;
    else if (FlagMatches(f_quiet, argv[i])) quiet = 1;
    else if (!filename)      filename = argv[i];
    else if (!input_content) input_content = argv[i];
  }

  if (!filename) {
    fprintf(stderr, SGR_RED "Error:" SGR_RESET " Filename required.\n");
    return EXIT_FAILURE;
  }

  full_path = build_path(sub_dir, filename);
  if (!full_path) return EXIT_FAILURE;

  /* 4. Read from STDIN when no inline content was supplied */
  if (cmd != CMD_REMOVE && !input_content) {
    input_content = read_stdin_to_buffer();
    alloc_content = 1;
    if (!input_content) { free(full_path); return EXIT_FAILURE; }
  }

  /* 5. Execute */
  if (cmd == CMD_WRITE) {
    if (file_exists(full_path) && !force) {
      fprintf(stderr, SGR_RED "Error:" SGR_RESET " File exists. Use -f to overwrite.\n");
      if (alloc_content) free(input_content);
      free(full_path);
      return EXIT_FAILURE;
    }
    fp = fopen(full_path, "w");
    if (!fp) perror("fopen");
    else { fputs(input_content, fp); fclose(fp); if (!quiet) printf("%s\n", filename); }

  } else if (cmd == CMD_APPEND) {
    fp = fopen(full_path, "a");
    if (!fp) perror("fopen");
    else { fprintf(fp, "\n%s", input_content); fclose(fp); if (!quiet) printf("%s\n", filename); }

  } else if (cmd == CMD_REMOVE) {
    if (unlink(full_path) != 0) perror("unlink");
  }

  if (alloc_content) free(input_content);
  free(full_path);
  return EXIT_SUCCESS;
}

/* =========================================================================
 * Low-level utilities
 * ====================================================================== */

static const char *find_last_path_separator(const char *path) {
  const char *fwd  = strrchr(path, '/');
  const char *back = strrchr(path, '\\');
  if (!fwd)  return back;
  if (!back) return fwd;
  return (fwd > back) ? fwd : back;
}

static int extract_order_number(const char *filename) {
  if (strncmp(filename, "ordered.", 8) != 0) return -1;
  char *endptr;
  long num = strtol(filename + 8, &endptr, 10);
  if (endptr == filename + 8 || num < 0 || num > INT_MAX) return -1;
  return (int)num;
}

/* Returns a pointer to the suffix of an ordered filename — the part after
 * "ordered.<digits>." — so that same-group files sort by intended name
 * rather than by digit formatting (e.g. "001" vs "01" vs "1").
 * For non-ordered filenames, returns the full name unchanged. */
static const char *extract_suffix(const char *filename) {
  if (strncmp(filename, "ordered.", 8) != 0) return filename;
  const char *p = filename + 8;
  while (isdigit((unsigned char)*p)) p++;
  return (*p == '.') ? p + 1 : p;
}

static int compare_ordered_files(const void *a, const void *b) {
  const FileEntry *fa = (const FileEntry *)a;
  const FileEntry *fb = (const FileEntry *)b;
  if (fa->order_num != fb->order_num) return fa->order_num - fb->order_num;
  return strcmp(extract_suffix(fa->name), extract_suffix(fb->name));
}

static int compare_unordered(const void *a, const void *b) {
  const FileEntry *fa = (const FileEntry *)a;
  const FileEntry *fb = (const FileEntry *)b;
  if (fa->order_num != fb->order_num) return fa->order_num - fb->order_num;
  return strcmp(fa->name, fb->name);
}

static char *read_file_contents(const char *directory, const char *filename, size_t *size) {
  char filepath[PATH_MAX];
  snprintf(filepath, sizeof(filepath), "%s/%s", directory, filename);

  FILE *file = fopen(filepath, "r");
  if (!file) {
    fprintf(stderr, "Error opening file '%s': %s\n", filepath, strerror(errno));
    return NULL;
  }

  struct stat st;
  if (stat(filepath, &st) != 0) {
    fprintf(stderr, "Error getting file size for '%s': %s\n", filepath, strerror(errno));
    fclose(file);
    return NULL;
  }

  char *contents = malloc((size_t)st.st_size + 1);
  if (!contents) {
    fprintf(stderr, "Error allocating memory for file '%s'\n", filepath);
    fclose(file);
    return NULL;
  }

  size_t bytes_read = fread(contents, 1, (size_t)st.st_size, file);
  if (bytes_read != (size_t)st.st_size) {
    fprintf(stderr, "Error reading file '%s': %s\n", filepath, strerror(errno));
    free(contents);
    fclose(file);
    return NULL;
  }

  contents[bytes_read] = '\0';
  *size = bytes_read;
  fclose(file);
  return contents;
}

static char *ensure_buffer_capacity(char *buffer, size_t *capacity, size_t needed) {
  if (needed <= *capacity) return buffer;

  size_t new_capacity = *capacity;
  while (new_capacity < needed) new_capacity *= 2;

  char *nb = realloc(buffer, new_capacity);
  if (!nb) {
    fprintf(stderr, "Failed to reallocate buffer to %zu bytes\n", new_capacity);
    free(buffer);
    return NULL;
  }

  *capacity = new_capacity;
  return nb;
}

static size_t count_lines(const char *content, size_t size) {
  size_t count = 0;
  for (size_t i = 0; i < size; i++)
    if (content[i] == '\n') count++;
  if (size > 0 && content[size - 1] != '\n') count++;
  return count;
}

/* =========================================================================
 * Edit subcommand helpers
 * ====================================================================== */

static int FlagMatches(FlagDef flag, const char *argument) {
  return (
    (flag.short_name && strcmp(flag.short_name, argument) == 0) ||
    (flag.long_name  && strcmp(flag.long_name,  argument) == 0)
  ) ? 1 : 0;
}

static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static char *read_stdin_to_buffer(void) {
  size_t capacity = 1024, size = 0;
  char  *buffer   = malloc(capacity);
  int    ch;

  if (!buffer) return NULL;

  while ((ch = getchar()) != EOF) {
    if (size + 1 >= capacity) {
      capacity *= 2;
      buffer = realloc(buffer, capacity);
      if (!buffer) return NULL;
    }
    buffer[size++] = (char)ch;
  }

  buffer[size] = '\0';
  return buffer;
}

static char *build_path(const char *sub_dir, const char *filename) {
  const char *base = getenv("SCRIPTSORT_DIR");
  const char *home = getenv("HOME");
  char       *full_path;
  size_t      len;

  if (!base) {
    if (!home) {
      fprintf(stderr, SGR_RED "Error:" SGR_RESET " SCRIPTSORT_DIR and HOME are unset.\n");
      return NULL;
    }
    len = strlen(home) + strlen("/.local/scripts/") + strlen(sub_dir) + strlen("/") + strlen(filename) + 1;
    full_path = calloc(len, sizeof(char));
    if (full_path) sprintf(full_path, "%s/.local/scripts/%s/%s", home, sub_dir, filename);
  } else {
    len = strlen(base) + strlen("/") + strlen(sub_dir) + strlen("/") + strlen(filename) + 1;
    full_path = calloc(len, sizeof(char));
    if (full_path) sprintf(full_path, "%s/%s/%s", base, sub_dir, filename);
  }

  return full_path;
}
