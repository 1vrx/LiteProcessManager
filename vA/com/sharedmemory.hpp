#pragma once
#include <ntifs.h>
#include "../../shared/protocol.h"
namespace com {
NTSTATUS start();
void stop();
}
