#include "Application/IApplicationApi.h"
#include "Core/Common/Logging.h"
#include <iostream>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    CPS_LOG_INFO("CarPlayStation", "CarPlayStation Receiver Initializing (Windows C++20)...");
    CPS_LOG_INFO("CarPlayStation", "Architecture layers: UI -> Application -> Core -> Lib");

    return 0;
}

