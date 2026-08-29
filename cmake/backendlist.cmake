# The engine-free half. Every file here builds without CommonLibF4 and without F4SE,
# so tools/subrect-harness can exercise a backend with no game running. A file that
# appears both here and in sourcelist.cmake is a duplicate-symbol link failure.
set(backend_headers
	src/Upscaler/Backend/BackendTypes.h
	src/Upscaler/Backend/IBackend.h
	src/Upscaler/Backend/BackendRegistry.h
	src/Upscaler/Backend/NullBackend.h
	src/Upscaler/Backend/NgxD3D11.h
	src/Upscaler/Backend/Streamline.h
	src/Upscaler/Backend/FidelityFxD3D11.h
)

set(backend_sources
	src/Upscaler/Backend/BackendRegistry.cpp
	src/Upscaler/Backend/NullBackend.cpp
	src/Upscaler/Backend/NgxD3D11.cpp
	src/Upscaler/Backend/Streamline.cpp
	src/Upscaler/Backend/FidelityFxD3D11.cpp
)
