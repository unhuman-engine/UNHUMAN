# SimulationDrone assets

Everything this project loads lives under this folder. Opening the project mounts
it as `game:`, so scenes, models and shaders are referenced with project-relative
paths (for example `Models/drone.glb`) instead of absolute ones.

Suggested layout:

```
Assets/
  Scenes/     *.unhuman scene files
  Models/     drone meshes (glTF/GLB)
  Textures/   materials
  Environments/
```

Save scenes here with File > Save Scene As... (the dialog opens in this folder),
then point the project at one of them with `StartScene: Assets/Scenes/Main.unhuman`
in `Project.uheproject` so the editor reopens it automatically.
