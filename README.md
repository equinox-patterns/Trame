# Trame

Trame takes a pattern's pieces and decides where they go on a length of fabric,
then writes the plan.

It is a library, not an application: it has no window of its own. It is the
nesting engine shared by Ostara and Rabnest, written by the same author and
published under the same licence.

## License

Trame is free software: you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

Trame is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

The full text is in `LICENSE`. The people who wrote it, and the third-party work
it embeds, are listed in `AUTHORS.txt`.

## Requirements

C++17, CMake 3.15 or later, Qt 6. Clipper2 is vendored in `third_party/` and
needs nothing else.
