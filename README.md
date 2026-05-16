[![Русский](https://img.shields.io/badge/Русский-%F0%9F%87%B7%F0%9F%87%BA-green?style=for-the-badge)](README_ru.md)

# S2-Launcher: Server Side Modding Framework for Source 2

It is a cutting-edge server-side modding framework designed specifically for Counter-Strike 2 servers. This project takes modding to the next level by implementing a Plugify Plugin & Package Manager on top of a Metamod Source Plugin. This unique approach allows developers to create plugins that seamlessly interact with the game server, using Plugify language modules to enable the creation of maintainable and testable code.

[Come and join our Discord](https://discord.gg/rX9TMmpang)

## Key Features

1. **Seamless Integration:**
   S2-Launcher seamlessly integrates with Counter-Strike 2 servers, providing a robust framework for server-side modding. Harness the power of Plugify's versatility to enhance and extend your server's functionality.

2. **Multi-Language Support:**
   Develop plugins in your language of choice, whether it's C++, C#, Python, or other supported languages. Plugify introduces language modules, breaking down language barriers and offering flexibility in mod development.

3. **Plugify Plugin & Package Manager:**
   Utilize the Plugify Plugin & Package Manager to effortlessly manage and distribute your server-side mods. Install, update, and remove packages with ease, ensuring a smooth and efficient modding experience.

4. **Maintainable and Testable Code:**
   Leverage Plugify to create maintainable and testable code for your mods. Enjoy the benefits of a structured and modular development approach, enhancing the overall reliability and quality of your server-side modifications.
   [Read more about Plugify here](https://github.com/untrustedmodders/plugify)

## Getting Started

1. **Installation:**
   - Download the latest build from [here](https://github.com/untrustedmodders/plugify-source2-launcher/releases/).
      - `plugify-source2-launcher` — standalone executable (launcher mode)
      - `plugify-source2-runtime` — shared library loaded automatically by CS2 (autoload mode)
   - Detailed installation instructions can be found in the [Installation Guide](https://plugify.net/use-cases/standalone-launcher/installation).

2. **Usage:**
   - Explore the [Usage Guide](https://plugify.net/essentials/installation) for detailed instructions on creating server-side mods, installing language modules, and maximizing the potential of Plugify.

3. **Examples:**
   - Check out the [Examples](https://plugify.net/plugins/s2sdk/guides/) directory for samples and use cases to kickstart your modding journey with Plugify.

## Loading Modes

S2-Launcher supports two ways to load Plugify into your CS2 server:

1. **Executable (Launcher mode):**
   Run `s2launcher` as a standalone executable alongside your server process. Gives you full control over the launch lifecycle.

2. **Shared Library (Autoload mode):**
   Place `libserver_valve.so` (Linux) into your CS2 game folder and the engine will load Plugify automatically on server startup — no separate process needed.

   > **When to use which?**
   > Use autoload if you want zero-friction deployment with no extra process to manage. Use the launcher if you need to control startup order or run outside of a CS2 server context.

## Requirements
- [CMake](https://cmake.org/download/) - version 3.14 or higher

## Documentation

Refer to the [official documentation](https://plugify.net/use-cases/metamod-plugin/) for in-depth information about Plugify's features, configuration options, and advanced modding techniques.

## Contributing

We welcome contributions! See [CONTRIBUTING.md](https://plugify.net/community-support/join-community) for information on how to contribute to Plugify and be part of the thriving modding community.

## License

S2-Launcher is licensed under the [MIT](LICENSE).
