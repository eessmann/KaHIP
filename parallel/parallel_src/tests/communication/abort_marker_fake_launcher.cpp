#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  auto fixed_broadcast = false;
  for (auto index = 1; index < argc; ++index) {
    fixed_broadcast =
        fixed_broadcast || std::string_view{argv[index]} == "fixed-broadcast";
  }

  if (fixed_broadcast) {
    std::cerr << "observed fixed-broadcast MPI_Abort on affected communicator\n"
              << "MPI backend failure: synthetic fixed-broadcast diagnostic\n";
  } else {
    std::cerr << "observed vertex-cut MPI_Abort on affected communicator\n"
              << "MPI backend failure: synthetic vertex-cut diagnostic\n";
  }
  return 86;
}
