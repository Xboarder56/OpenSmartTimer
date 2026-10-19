Import("env")

artifact = env.GetProjectOption("custom_firmware_artifact")
if not artifact or "/" in artifact or "\\" in artifact:
    raise RuntimeError("custom_firmware_artifact must be a simple file name")

env.Replace(PROGNAME=artifact)
