# Tester for Linux
FROM --platform=linux/amd64 ubuntu:24.04

# Avoid interactive prompts during package install
ENV DEBIAN_FRONTEND=noninteractive

# Install base dependencies: build tools, curl, git, and Node.js
RUN apt-get update && apt-get install -y \
    curl \
    git \
    gcc \
    make \
    binutils \
    ca-certificates \
    gnupg \
    && rm -rf /var/lib/apt/lists/*

# Install Node.js (LTS) via NodeSource
RUN curl -fsSL https://deb.nodesource.com/setup_lts.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

# Install Codex CLI globally
RUN npm install -g @openai/codex

# Set up a non-root user for realistic experience
RUN useradd -m -s /bin/bash user
USER user
WORKDIR /home/user

# Add ~/.local/bin to PATH
ENV PATH="/home/user/.local/bin:${PATH}"

# Download remapper from GitHub releases
RUN mkdir -p ~/.local/bin && curl -L -o ~/.local/bin/remapper \
    https://github.com/zafnz/remapper/releases/latest/download/remapper-Linux-x86_64 \
    && chmod +x ~/.local/bin/remapper

CMD ["/bin/bash"]
