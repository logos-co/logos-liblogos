#include <cstdio>
#include <cstdlib>

#include "../src/logos_core.h"

static void print_string_array(const char* title, char** arr) {
	printf("%s\n", title);
	if (arr == nullptr || arr[0] == nullptr) {
		printf("  (none)\n");
		return;
	}
	for (int i = 0; arr[i] != nullptr; i++) {
		printf("  - %s\n", arr[i]);
	}
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	printf("logos-cli: starting...\n");

	logos_core_init(0, nullptr);
	logos_core_set_mode(LOGOS_MODE_LOCAL);
	printf("logos-cli: SDK mode set to Local\n");

	const char* plugins_dir = getenv("LOGOS_PLUGINS_DIR");
	if (plugins_dir != nullptr && plugins_dir[0] != '\0') {
		printf("Setting plugins dir: %s\n", plugins_dir);
		logos_core_set_plugins_dir(plugins_dir);
	}

	logos_core_start();

	char** known = logos_core_get_known_plugins();
	char** loaded = logos_core_get_loaded_plugins();

	print_string_array("Known plugins:", known);
	print_string_array("Loaded plugins:", loaded);

	logos_core_cleanup();

	printf("logos-cli: done.\n");
	return 0;
}
