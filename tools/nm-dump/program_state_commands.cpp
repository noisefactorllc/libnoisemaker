#include "program_state_commands.h"

namespace nm_dump {
int run_program_state_full(int argc, char** argv);

int run_program_state(int argc, char** argv) {
    return run_program_state_full(argc, argv);
}
}  // namespace nm_dump
