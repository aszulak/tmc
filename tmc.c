#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ENV_IFACE_PRIMARY "TMC_PRIMARY"
#define ENV_IFACE_SECONDARY "TMC_SECONDARY"
#define ENV_CLIENT_BIN "TMC_CLIENT_BIN"
#define SHARED_LIB_NAME "libtmc.so"
#define DEFAULT_CLIENT_REL                                                     \
  "/.local/share/CipSoft GmbH/Tibia/packages/Tibia/bin/client"

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s -p <iface> -s <iface> [-c <client_path>] [-- <args...>]\n"
          "\n"
          "Options:\n"
          "  -p, --primary <iface>    Primary network interface (prod-cf-*)\n"
          "  -s, --secondary <iface>  Secondary network interface (prod-lb-*)\n"
          "  -c, --client <path>      Path to Tibia client binary\n"
          "  -h, --help               Show this help message\n",
          prog);
}

static void resolve_shared_lib(char *out, size_t size) {
  char exe[PATH_MAX] = {0};
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len > 0) {
    char *slash = strrchr(exe, '/');
    if (slash) {
      *slash = '\0';
      int n = snprintf(out, size, "%s/%s", exe, SHARED_LIB_NAME);
      if (n > 0 && (size_t)n < size && access(out, R_OK) == 0) {
        return;
      }
    }
  }
  snprintf(out, size, "%s", SHARED_LIB_NAME);
}

static void resolve_client_bin(char *out, size_t size) {
  const char *env_bin = getenv(ENV_CLIENT_BIN);
  if (env_bin && env_bin[0] != '\0') {
    snprintf(out, size, "%s", env_bin);
    return;
  }

  const char *home = getenv("HOME");
  if (home && home[0] != '\0') {
    int n = snprintf(out, size, "%s%s", home, DEFAULT_CLIENT_REL);
    if (n > 0 && (size_t)n < size && access(out, X_OK) == 0) {
      return;
    }
  }

  snprintf(out, size, "./client");
}

int main(int argc, char *argv[]) {
  const char *primary = NULL;
  const char *secondary = NULL;
  const char *client = NULL;

  static const struct option long_opts[] = {
      {"primary", required_argument, NULL, 'p'},
      {"secondary", required_argument, NULL, 's'},
      {"client", required_argument, NULL, 'c'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "+p:s:c:h", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'p':
      primary = optarg;
      break;
    case 's':
      secondary = optarg;
      break;
    case 'c':
      client = optarg;
      break;
    case 'h':
      usage(argv[0]);
      return EXIT_SUCCESS;
    default:
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (!primary || !secondary || primary[0] == '\0' || secondary[0] == '\0') {
    fprintf(stderr,
            "[tmc] error: both --primary and --secondary are required\n");
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (strlen(primary) >= IFNAMSIZ || strlen(secondary) >= IFNAMSIZ) {
    fprintf(stderr, "[tmc] error: interface name exceeds %d chars\n",
            IFNAMSIZ - 1);
    return EXIT_FAILURE;
  }

  char default_client[PATH_MAX] = {0};
  if (!client || client[0] == '\0') {
    resolve_client_bin(default_client, sizeof(default_client));
    client = default_client;
  }

  char lib_path[PATH_MAX] = {0};
  resolve_shared_lib(lib_path, sizeof(lib_path));

  char preload[PATH_MAX * 2] = {0};
  const char *old_preload = getenv("LD_PRELOAD");
  if (old_preload && old_preload[0] != '\0') {
    snprintf(preload, sizeof(preload), "%s:%s", lib_path, old_preload);
  } else {
    snprintf(preload, sizeof(preload), "%s", lib_path);
  }

  if (setenv(ENV_IFACE_PRIMARY, primary, 1) != 0 ||
      setenv(ENV_IFACE_SECONDARY, secondary, 1) != 0 ||
      setenv("LD_PRELOAD", preload, 1) != 0) {
    fprintf(stderr, "[tmc] error: setenv failed: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  int extra = argc - optind;
  char **exec_argv = calloc((size_t)(extra + 2), sizeof(char *));
  if (!exec_argv) {
    fprintf(stderr, "[tmc] error: allocation failed\n");
    return EXIT_FAILURE;
  }

  exec_argv[0] = (char *)client;
  for (int i = 0; i < extra; ++i) {
    exec_argv[i + 1] = argv[optind + i];
  }

  fprintf(stderr,
          "[tmc] exec: '%s' | primary='%s' secondary='%s' preload='%s'\n",
          client, primary, secondary, preload);

  execvp(client, exec_argv);
  fprintf(stderr, "[tmc] error: failed to execute '%s': %s\n", client,
          strerror(errno));
  free(exec_argv);
  return EXIT_FAILURE;
}
