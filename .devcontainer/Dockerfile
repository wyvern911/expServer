FROM mcr.microsoft.com/devcontainers/base:ubuntu

# Install C toolchain, GDB, and Docker CLI (for DooD)
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
    gcc gdb make git docker.io curl

# Install Node.js 18+ (Required for the tester utility)
RUN curl -fsSL https://deb.nodesource.com/setup_18.x | bash - \
    && apt-get install -y nodejs