# Qt license texts

Live2D Mate links Qt 6 **dynamically**, using the open source build, which is
licensed under the **GNU Lesser General Public License v3.0**. LGPL v3 is
written as a set of additional permissions on top of the **GNU General Public
License v3.0**, so both texts are required, and both are included here:

- `LGPL-3.0.txt` — GNU Lesser General Public License, version 3
- `GPL-3.0.txt` — GNU General Public License, version 3

Both are the verbatim texts published by the Free Software Foundation at
<https://www.gnu.org/licenses/>, reproduced here unchanged — which the
documents themselves permit: "Everyone is permitted to copy and distribute
verbatim copies of this license document, but changing it is not allowed."

They live in the repository rather than being copied out of a Qt installation
at build time. The CI Qt install (aqtinstall) ships module binaries only and
never carries these texts, so the old copy step found nothing on every run and
quietly produced packages with no license texts at all.

Qt itself is not part of this repository. For which Qt version this build uses,
and where to obtain the corresponding Qt sources, see `THIRD_PARTY_NOTICES.md`.
