#include <stdio.h>
#include <stdlib.h>

#include "../src/logos_core.h"

static void print_string_array(const char* title, char** arr) {
	printf("%s\n", title);
	if (arr == NULL || arr[0] == NULL) {
		printf("  (none)\n");
		return;
	}
	for (int i = 0; arr[i] != NULL; i++) {
		printf("  - %s\n", arr[i]);
	}
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	printf("logos-cli: starting...\n");

	// Initialize core (no need to pass real argv for this simple check)
	logos_core_init(0, NULL);

	// Optional: set a custom plugins directory via LOGOS_PLUGINS_DIR env var
	// If not set, lib defaults to a relative path resolved internally
	const char* plugins_dir = getenv("LOGOS_PLUGINS_DIR");
	if (plugins_dir != NULL && plugins_dir[0] != '\0') {
		printf("Setting plugins dir: %s\n", plugins_dir);
		logos_core_set_plugins_dir(plugins_dir);
	}

	// Start core services and plugin discovery
	logos_core_start();

	// Query known and loaded plugins
	char** known = logos_core_get_known_plugins();
	char** loaded = logos_core_get_loaded_plugins();

	print_string_array("Known plugins:", known);
	print_string_array("Loaded plugins:", loaded);

	// Note: The arrays returned by the C API are allocated internally by the library.
	// The current API does not expose a C function to free them safely from C code.
	// Since this is a short-lived CLI, we intentionally do not free them here.

	// Cleanup core
	logos_core_cleanup();

	printf("logos-cli: done.\n");
	return 0;
}


