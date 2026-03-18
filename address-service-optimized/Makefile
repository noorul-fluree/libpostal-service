.PHONY: all build release debug test clean install docker run

# Default target
all: release

# Release build
release:
	@mkdir -p build && cd build && \
	cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_LLM=OFF -DENABLE_TESTS=OFF && \
	make -j$$(nproc)
	@echo "\n✓ Build complete: build/address-service"

# Release build with LLM support
release-llm:
	@mkdir -p build && cd build && \
	cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_LLM=ON -DENABLE_TESTS=OFF && \
	make -j$$(nproc)
	@echo "\n✓ Build complete (with LLM): build/address-service"

# Debug build
debug:
	@mkdir -p build-debug && cd build-debug && \
	cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_LLM=OFF -DENABLE_TESTS=ON && \
	make -j$$(nproc)
	@echo "\n✓ Debug build complete: build-debug/address-service"

# Build and run tests
test:
	@mkdir -p build-test && cd build-test && \
	cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_LLM=OFF -DENABLE_TESTS=ON && \
	make -j$$(nproc) && \
	./address-service-tests

# Clean build artifacts
clean:
	@rm -rf build build-debug build-test
	@echo "✓ Cleaned"

# Install to /opt/address-service
install: release
	@sudo mkdir -p /opt/address-service/bin
	@sudo cp build/address-service /opt/address-service/bin/
	@sudo cp config/config.json /etc/address-service/config.json 2>/dev/null || true
	@echo "✓ Installed to /opt/address-service/bin/"

# Docker build
docker:
	docker build -t address-service:latest .

# Docker build with LLM
docker-llm:
	docker build -t address-service:latest --build-arg ENABLE_LLM=ON .

# Run locally
run: release
	./build/address-service --config config/config.json

# Docker compose up
up:
	docker compose up -d address-service nginx

# Docker compose down
down:
	docker compose down

# Show project stats
stats:
	@echo "=== Source Files ==="
	@find src -name '*.cc' -o -name '*.h' | wc -l
	@echo "=== Test Files ==="
	@find tests -name '*.cc' | wc -l
	@echo "=== Total C++ Lines ==="
	@find src tests -name '*.cc' -o -name '*.h' | xargs wc -l | tail -1
	@echo "=== Config Files ==="
	@find config -type f | wc -l
