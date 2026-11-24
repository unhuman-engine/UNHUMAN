# GameSnake assets

Everything this project loads lives under this folder. Opening the project mounts
it as `game:`, so scenes, sprites, models and sounds are referenced with
project-relative paths (for example `Textures/snake_head.png`) instead of
absolute ones.

Suggested layout:

```
Assets/
  Scenes/     *.unhuman scene files
  Textures/   sprites and sprite sheets
  Models/     glTF/GLB models
  Audio/
```

Save scenes here with File > Save Scene As... (the dialog opens in this folder),
then point the project at one of them with `StartScene: Assets/Scenes/Main.unhuman`
in `Project.uheproject` so the editor reopens it automatically.
