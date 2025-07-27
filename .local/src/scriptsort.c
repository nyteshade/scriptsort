/**
 * scriptsort.c
 *
 * This program sorts shell script files in a directory according to specific ordering rules:
 * 1. First, ordered files with numbers < 50 (ordered.[0-4][0-9].*)
 * 2. Then, unordered files (files not matching ordered.*)
 * 3. Finally, ordered files with numbers >= 50 (ordered.[5-9][0-9].* or ordered.[1-9][0-9][0-9].*)
 *
 * Alternatively, it supports a --init flag that will generate a script to make usage
 * even simpler. Simply add the following to your .zsh/.bashrc/.profile file.
 *
 *   source <(/path/to/scriptsort /path/to/dir --init)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>

#define MAX_FILES 1000
#define MAX_FILENAME 256
#define INITIAL_BUFFER_SIZE 4096

typedef char *String;
typedef unsigned char Boolean;
const Boolean Truth = 1;
const Boolean Falsehood = 0;

// Structure to hold file information
typedef struct {
  char name[MAX_FILENAME];
  int order_num;  // -1 for unordered files
  unsigned int bytesize;
} FileEntry;

// Function prototypes
static void print_usage(const char *program_name);
static int extract_order_number(const char *filename);
static int compare_ordered_files(const void *a, const void *b);
static int compare_unordered(const void *a, const void *b);
static int wal_stricmp(const char *a, const char *b);
static char* read_file_contents(const char* directory, const char* filename, size_t* size);
static char* ensure_buffer_capacity(char* buffer, size_t* current_capacity, size_t needed_size);

int main(int argc, char *argv[]) {
  unsigned int cutoff_count = 50;

  if (argc < 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  String  buffer = NULL;
  Boolean init = Falsehood;
  Boolean bundle = Falsehood;
  Boolean debugtext = Falsehood;

  for (int i = 2; i < argc; i++) {
    if (wal_stricmp(argv[i], "--init") == 0)
      init = Truth;
    else if (wal_stricmp(argv[i], "--bundle") == 0)
      bundle = Truth;
    else if (wal_stricmp(argv[i], "--debug") == 0)
      debugtext = Truth;
    else if (wal_stricmp(argv[i], "--cutoff") == 0 && (i + 1 < argc)) {
      int count = atoi(argv[++i]);

      if (count <= 0) {
        printf("The cutoff number defaults to 50, but must be a number\n");
        printf("that is greater than 0 in number.\n");
        return 1;
      }

      cutoff_count = count;
    }
  }

  DIR *dir = opendir(argv[1]);
  if (!dir) {
    fprintf(stderr, "Error opening directory '%s': %s\n", argv[1], strerror(errno));
    return EXIT_FAILURE;
  }

  // Arrays to store different categories of files
  FileEntry lower_files[MAX_FILES] = {0};
  FileEntry upper_files[MAX_FILES] = {0};
  FileEntry unordered_files[MAX_FILES] = {0};
  int lower_count = 0, upper_count = 0, unordered_count = 0, bytesize = 0;
  unsigned char joiner = 0;

  // Read directory entries
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    // Skip files starting with "skip."
    if (strncmp(entry->d_name, "skip.", 5) == 0) {
      continue;
    }

    int order_num = extract_order_number(entry->d_name);
    FileEntry new_entry = {0};

    strncpy(new_entry.name, entry->d_name, MAX_FILENAME - 1);
    new_entry.order_num = order_num;
    new_entry.bytesize = strlen(new_entry.name);

    bytesize += new_entry.bytesize + 2; // capture newline + null size as well

    // Categorize files based on their order number
    if (order_num >= 0 && order_num < cutoff_count) {
      if (lower_count < MAX_FILES) {
        lower_files[lower_count++] = new_entry;
      }
    } else if (order_num >= cutoff_count) {
      if (upper_count < MAX_FILES) {
        upper_files[upper_count++] = new_entry;
      }
    } else {  // unordered files
      if (unordered_count < MAX_FILES) {
        unordered_files[unordered_count++] = new_entry;
      }
    }
  }
  closedir(dir);

  // Sort each category
  qsort(lower_files, lower_count, sizeof(FileEntry), compare_ordered_files);
  qsort(upper_files, upper_count, sizeof(FileEntry), compare_ordered_files);
  qsort(unordered_files, unordered_count, sizeof(FileEntry), compare_unordered);

  if (bundle) {
    size_t buffer_capacity = INITIAL_BUFFER_SIZE;
    size_t current_size = 0;
    size_t file_size;
    char* file_contents;

    buffer = malloc(buffer_capacity);
    if (!buffer) {
      fprintf(stderr, "Failed to allocate initial buffer\n");
      return EXIT_FAILURE;
    }
    buffer[0] = '\0';

    // Process all files in order and concatenate their contents
    for (int i = 0; i < lower_count; i++) {
      file_contents = read_file_contents(argv[1], lower_files[i].name, &file_size);
      if (!file_contents) continue;

      // Ensure buffer has enough space for contents + newline + null
      buffer = ensure_buffer_capacity(buffer, &buffer_capacity, current_size + file_size + 2);
      if (!buffer) {
        free(file_contents);
        return EXIT_FAILURE;
      }

      strcat(buffer + current_size, file_contents);
      current_size += file_size;
      buffer[current_size++] = '\n';
      buffer[current_size] = '\0';
      free(file_contents);
    }

    for (int i = 0; i < unordered_count; i++) {
      file_contents = read_file_contents(argv[1], unordered_files[i].name, &file_size);
      if (!file_contents) continue;

      buffer = ensure_buffer_capacity(buffer, &buffer_capacity, current_size + file_size + 2);
      if (!buffer) {
        free(file_contents);
        return EXIT_FAILURE;
      }

      strcat(buffer + current_size, file_contents);
      current_size += file_size;
      buffer[current_size++] = '\n';
      buffer[current_size] = '\0';
      free(file_contents);
    }

    for (int i = 0; i < upper_count; i++) {
      file_contents = read_file_contents(argv[1], upper_files[i].name, &file_size);
      if (!file_contents) continue;

      buffer = ensure_buffer_capacity(buffer, &buffer_capacity, current_size + file_size + 2);
      if (!buffer) {
        free(file_contents);
        return EXIT_FAILURE;
      }

      strcat(buffer + current_size, file_contents);
      current_size += file_size;
      buffer[current_size++] = '\n';
      buffer[current_size] = '\0';
      free(file_contents);
    }

    if (debugtext) {
      printf(
        "local start_time=%s\n",
        "$(command 2>&1 >/dev/null -v ms && ms || printf '0')"
      );
    }

    printf("%s\n", buffer);

    if (debugtext) {
      printf(
        "local end_time=%s\n",
        "$(command 2>&1 >/dev/null -v ms && ms || printf '0')"
      );
      printf("export SCRIPTSORT_ELAPSED=$(($end_time - $start_time))\n");
    }
  } else {
    buffer = calloc(1, bytesize);
    if (!buffer) {
      fprintf(stderr, "Cannot allocate buffer of %d byte(s)\n", bytesize);
      return EXIT_FAILURE;
    }

    // If init is true, we create a shell array. We need spaces instead
    joiner = (init == Truth) ? ' ' : '\n';

    // Output files in the required order
    for (int i = 0; i < lower_count; i++) {
      sprintf(buffer, "%s%s%c", buffer, lower_files[i].name, joiner);
    }

    for (int i = 0; i < unordered_count; i++) {
      sprintf(buffer, "%s%s%c", buffer, unordered_files[i].name, joiner);
    }

    for (int i = 0; i < upper_count; i++) {
      sprintf(buffer, "%s%s%c", buffer, upper_files[i].name, joiner);
    }

    if (init) {
      const char *debugStart = debugtext ? "    printf \"Sourcing \\\"${scriptpath}\\\"...\"\n" : "\n";
      const char *debugEnd = debugtext ? "    printf \"done\\n\"\n" : "";
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

      printf(script, buffer, timer, debugStart, timer, debugEnd, argv[1]);
    }
    else {
      printf("%s", buffer);
    }
  }

  if (buffer) {
    free(buffer);
  }

  return EXIT_SUCCESS;
}

/**
 * Finds the last path separator in a string, handling both forward and backward slashes
 * Returns NULL if no separator is found
 */
static const char* find_last_path_separator(const char *path) {
  const char *last_forward = strrchr(path, '/');
  const char *last_backward = strrchr(path, '\\');

  if (!last_forward) return last_backward;
  if (!last_backward) return last_forward;

  // Return whichever separator appears later in the string
  return (last_forward > last_backward) ? last_forward : last_backward;
}

/**
 * Prints program usage information
 */
static void print_usage(const char *program_name) {
  // Find last occurrence of any path separator
  const char *basename = program_name;
  const char *last_sep = find_last_path_separator(program_name);

  // If separator found, move pointer after it
  if (last_sep != NULL) {
    basename = last_sep + 1;
  }

  fprintf(
    stderr,
    "Usage: \x1b[35;1m%s\x1b[22;39m <directory_path> [--init OR --bundle]\n"
    "where\n"
    "  \x1b[33m<directory_path>\x1b[39m - \x1b[2mpath to scripts to order\x1b[22m\n"
    "  \x1b[34m--init\x1b[39m - \x1b[2mcreates a string that can be sourced\x1b[22m\n"
    "  \x1b[34m--bundle\x1b[39m - \x1b[2mconcatenates all scripts to single string\x1b[22m\n\n"
    "Given a directory structure like the following, anything not\n"
    "prefixed with 'ordered.', followed by a number, will be\n"
    "executed in a specific order.\n\n"
    "The order is\n"
    "  1. ordered.(0-49).(anything)\n"
    "  2. \x1b[3m(files not prefixed with ordered)\x1b[23m\n"
    "  3. ordered.(50+).(anything)\n\n"
    "So in a directory with 'ordered.01.first','fn.a','fn.b', and 'ordered.52.last'\n"
    "the files scriptsort will print:\n"
    "  ordered.01.first\n"
    "  fn.a\n"
    "  fn.b\n"
    "  ordered.52.last\n\n"
    "To make this simpler, simply add this to the bottom of your startup script\n"
    "  source <(scriptsort /path/to/dir --init)\n\n",
    basename
  );
}

/**
 * Extracts the order number from a filename
 * Returns the order number if found, -1 otherwise
 */
static int extract_order_number(const char *filename) {
  if (strncmp(filename, "ordered.", 8) != 0) {
    return -1;
  }

  char *endptr;
  long num = strtol(filename + 8, &endptr, 10);

  if (endptr == filename + 8 || num < 0 || num > INT_MAX) {
    return -1;
  }

  return (int)num;
}

/**
 * Comparison function for files with order numbers < 50
 */
static int compare_ordered_files(const void *a, const void *b) {
  const FileEntry *fa = (const FileEntry *)a;
  const FileEntry *fb = (const FileEntry *)b;

  if (fa->order_num != fb->order_num) {
    return fa->order_num - fb->order_num;
  }
  return strcmp(fa->name, fb->name);
}

/**
 * Comparison function for sorting files based on their order numbers.
 * This function is used for both lt50 and ge50 comparisons as they share
 * the same logic - comparing first by order number, then by name.
 *
 * @param a Pointer to the first FileEntry structure
 * @param b Pointer to the second FileEntry structure
 * @return Negative if a < b, positive if a > b, 0 if equal
 */
static int compare_unordered(const void *a, const void *b) {
  const FileEntry *fa = (const FileEntry *)a;
  const FileEntry *fb = (const FileEntry *)b;

  // First compare by order number
  if (fa->order_num != fb->order_num) {
    return fa->order_num - fb->order_num;
  }
  // If order numbers are equal, compare by name
  return strcmp(fa->name, fb->name);
}

/**
 * Case insensitive comparison function. If a is lexographically
 * less than b, then -1 is returned. If they are lexographically
 * equal, then 0 is returned. Lastly if a is lexographically
 * greater than b, then 1 is returned.
 *
 * All lexographic comparisons are case insensitive.
 */
static int wal_stricmp(const char *a, const char *b) {
  int ca, cb;

  do {
    ca = *((unsigned char *) a++);
    cb = *((unsigned char *) b++);
    ca = tolower(toupper(ca));
    cb = tolower(toupper(cb));
  } while (ca == cb && ca != '\0');

  return (ca == cb) ? 0 : (ca < cb) ? -1 : 1;
}

static char* read_file_contents(const char* directory, const char* filename, size_t* size) {
  char filepath[PATH_MAX];
  snprintf(filepath, sizeof(filepath), "%s/%s", directory, filename);

  FILE* file = fopen(filepath, "r");
  if (!file) {
    fprintf(stderr, "Error opening file '%s': %s\n", filepath, strerror(errno));
    return NULL;
  }

  // Get file size
  struct stat st;
  if (stat(filepath, &st) != 0) {
    fprintf(stderr, "Error getting file size for '%s': %s\n", filepath, strerror(errno));
    fclose(file);
    return NULL;
  }

  // Allocate buffer for file contents
  char* contents = malloc(st.st_size + 1);
  if (!contents) {
    fprintf(stderr, "Error allocating memory for file '%s'\n", filepath);
    fclose(file);
    return NULL;
  }

  // Read file contents
  size_t bytes_read = fread(contents, 1, st.st_size, file);
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

static char* ensure_buffer_capacity(char* buffer, size_t* current_capacity, size_t needed_size) {
  if (needed_size > *current_capacity) {
    size_t new_capacity = *current_capacity;
    while (new_capacity < needed_size) {
      new_capacity *= 2;
    }

    char* new_buffer = realloc(buffer, new_capacity);
    if (!new_buffer) {
      fprintf(stderr, "Failed to reallocate buffer to size %zu\n", new_capacity);
      free(buffer);
      return NULL;
    }

    buffer = new_buffer;
    *current_capacity = new_capacity;
  }
  return buffer;
}
