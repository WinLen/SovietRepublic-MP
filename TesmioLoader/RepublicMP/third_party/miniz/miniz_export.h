// miniz_export.h — заменяет CMake-генерируемый файл того же имени (в
// исходниках miniz этот файл собирается через generate_export_header() при
// сборке miniz как ОТДЕЛЬНОЙ dll — нам это не нужно: miniz компилируется
// статически прямо внутрь RepublicMP.dll, поэтому MINIZ_EXPORT — просто пусто.
#pragma once

#define MINIZ_EXPORT
