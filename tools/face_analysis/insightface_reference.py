#!/usr/bin/env python3
"""Reference SCRFD + ArcFace outputs from the pinned InsightFace 0.7.3 package.

The harness deliberately calls the upstream 0.7.3 SCRFD and ArcFace wrappers.
It is a validation tool only; product inference is implemented in C++.
"""

from __future__ import annotations

import argparse
import importlib
import importlib.metadata
import importlib.util
import json
import struct
import subprocess
import sys
import types
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort


def exif_orientation(path: Path) -> int:
    """Read JPEG EXIF orientation without letting the decoder rotate pixels."""
    with path.open("rb") as source:
        data = source.read(128 * 1024)
    if len(data) < 4 or data[:2] != b"\xff\xd8":
        return 1
    position = 2
    while position + 4 <= len(data):
        if data[position] != 0xFF:
            position += 1
            continue
        while position < len(data) and data[position] == 0xFF:
            position += 1
        if position >= len(data):
            break
        marker = data[position]
        position += 1
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            if marker == 0xD9:
                break
            continue
        if position + 2 > len(data):
            break
        length = struct.unpack_from(">H", data, position)[0]
        if length < 2 or position + length > len(data):
            break
        if marker == 0xDA:
            break
        payload = data[position + 2:position + length]
        position += length
        if marker != 0xE1 or not payload.startswith(b"Exif\x00\x00"):
            continue
        tiff = payload[6:]
        if len(tiff) < 8:
            return 1
        if tiff[:2] == b"II":
            endian = "<"
        elif tiff[:2] == b"MM":
            endian = ">"
        else:
            return 1
        if struct.unpack_from(endian + "H", tiff, 2)[0] != 42:
            return 1
        ifd_offset = struct.unpack_from(endian + "I", tiff, 4)[0]
        if ifd_offset + 2 > len(tiff):
            return 1
        count = struct.unpack_from(endian + "H", tiff, ifd_offset)[0]
        cursor = ifd_offset + 2
        for _ in range(count):
            if cursor + 12 > len(tiff):
                return 1
            tag, kind, items = struct.unpack_from(endian + "HHI", tiff, cursor)
            if tag == 0x0112 and kind == 3 and items >= 1:
                value = struct.unpack_from(endian + "H", tiff, cursor + 8)[0]
                return value if 1 <= value <= 8 else 1
            cursor += 12
        return 1
    return 1


def orient_bgr(image: np.ndarray, orientation: int) -> np.ndarray:
    if orientation == 2:
        return cv2.flip(image, 1)
    if orientation == 3:
        return cv2.rotate(image, cv2.ROTATE_180)
    if orientation == 4:
        return cv2.flip(image, 0)
    if orientation == 5:
        return cv2.transpose(image)
    if orientation == 6:
        return cv2.rotate(image, cv2.ROTATE_90_CLOCKWISE)
    if orientation == 7:
        return cv2.flip(cv2.transpose(image), -1)
    if orientation == 8:
        return cv2.rotate(image, cv2.ROTATE_90_COUNTERCLOCKWISE)
    return image


def load_oriented_bgr(path: Path) -> tuple[np.ndarray, int]:
    orientation = exif_orientation(path)
    decoded = cv2.imread(str(path), cv2.IMREAD_COLOR | cv2.IMREAD_IGNORE_ORIENTATION)
    if decoded is None:
        raise RuntimeError(f"OpenCV could not decode {path}")
    return orient_bgr(decoded, orientation), orientation


def import_reference_modules(source: Path | None):
    if source is None:
        package_version = importlib.metadata.version("insightface")
        if package_version != "0.7.3":
            raise RuntimeError(f"Expected InsightFace 0.7.3, got {package_version}")
        return (
            importlib.import_module("insightface.model_zoo.scrfd").SCRFD,
            importlib.import_module("insightface.model_zoo.arcface_onnx").ArcFaceONNX,
            importlib.import_module("insightface.utils.face_align").norm_crop,
        )

    source = source.resolve()
    if not all((source / name).is_file() for name in ("scrfd.py", "arcface_onnx.py", "face_align.py")):
        raise RuntimeError("--reference-source must contain scrfd.py, arcface_onnx.py, and face_align.py from InsightFace 0.7.3")
    package = types.ModuleType("insightface")
    package.__path__ = []
    model_zoo = types.ModuleType("insightface.model_zoo")
    model_zoo.__path__ = [str(source)]
    utils = types.ModuleType("insightface.utils")
    utils.__path__ = [str(source)]
    package.model_zoo = model_zoo
    package.utils = utils
    sys.modules["insightface"] = package
    sys.modules["insightface.model_zoo"] = model_zoo
    sys.modules["insightface.utils"] = utils

    def load_module(name: str, filename: str):
        spec = importlib.util.spec_from_file_location(name, source / filename)
        if spec is None or spec.loader is None:
            raise RuntimeError(f"Could not load reference source {filename}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
        return module

    align = load_module("insightface.utils.face_align", "face_align.py")
    utils.face_align = align
    scrfd = load_module("insightface.model_zoo.scrfd", "scrfd.py")
    arcface = load_module("insightface.model_zoo.arcface_onnx", "arcface_onnx.py")
    return scrfd.SCRFD, arcface.ArcFaceONNX, align.norm_crop


def reference(model_dir: Path, image_path: Path, device: str, source: Path | None) -> dict:
    if ort.__version__ != "1.30.0":
        raise RuntimeError(f"Expected ONNX Runtime 1.30.0, got {ort.__version__}")

    SCRFD, ArcFaceONNX, norm_crop = import_reference_modules(source)
    context_id = 0 if device == "cuda" else -1
    detector = SCRFD(model_file=str(model_dir / "det_10g.onnx"))
    detector.prepare(ctx_id=context_id, det_thresh=0.5, nms_thresh=0.4)
    recognizer = ArcFaceONNX(model_file=str(model_dir / "w600k_r50.onnx"))
    recognizer.prepare(ctx_id=context_id)
    image, orientation = load_oriented_bgr(image_path)
    boxes, keypoints = detector.detect(image, input_size=(640, 640), max_num=0)

    detections = []
    if keypoints is not None:
        for index, row in enumerate(boxes):
            x1, y1, x2, y2, score = [float(value) for value in row]
            points = keypoints[index].astype(np.float32, copy=False)
            aligned = norm_crop(image, landmark=points, image_size=112)
            raw = recognizer.get_feat(aligned)[0].astype(np.float64)
            norm = np.linalg.norm(raw)
            if not np.isfinite(norm) or norm <= 1e-12:
                embedding = []
                embedding_error = "Reference ArcFace output has zero or invalid norm"
            else:
                embedding = (raw / norm).astype(np.float32).tolist()
                embedding_error = ""
            detections.append({
                "x": x1,
                "y": y1,
                "width": x2 - x1,
                "height": y2 - y1,
                "score": score,
                "landmarks": [{"x": float(x), "y": float(y)} for x, y in points],
                "embedding_dimensions": len(embedding),
                "embedding_error": embedding_error,
                "embedding": embedding,
            })

    return {
        "status": "ok",
        "image": str(image_path),
        "width": int(image.shape[1]),
        "height": int(image.shape[0]),
        "orientation": orientation,
        "pipeline_version": "insightface-0.7.3-v1",
        "runtime_version": ort.__version__,
        "detector_provider": "+".join(detector.session.get_providers()),
        "recognizer_provider": "+".join(recognizer.session.get_providers()),
        "detections": detections,
    }


def compare(reference_result: dict, native_result: dict,
            box_tolerance: float, score_tolerance: float, embedding_cosine: float) -> list[str]:
    issues: list[str] = []
    reference_faces = reference_result["detections"]
    native_faces = native_result.get("detections", [])
    if len(reference_faces) != len(native_faces):
        return [f"detection count differs: reference={len(reference_faces)} native={len(native_faces)}"]

    for face_index, (expected, actual) in enumerate(zip(reference_faces, native_faces)):
        for key in ("x", "y", "width", "height"):
            difference = abs(float(expected[key]) - float(actual[key]))
            if difference > box_tolerance:
                issues.append(f"face {face_index} {key}: delta {difference:.6g} > {box_tolerance}")
        score_delta = abs(float(expected["score"]) - float(actual["score"]))
        if score_delta > score_tolerance:
            issues.append(f"face {face_index} score: delta {score_delta:.6g} > {score_tolerance}")
        expected_landmarks = expected.get("landmarks", [])
        actual_landmarks = actual.get("landmarks", [])
        if len(expected_landmarks) != 5 or len(actual_landmarks) != 5:
            issues.append(f"face {face_index} must have exactly five landmarks (reference={len(expected_landmarks)} native={len(actual_landmarks)})")
        for point_index, (expected_point, actual_point) in enumerate(zip(expected_landmarks, actual_landmarks)):
            for axis in ("x", "y"):
                delta = abs(float(expected_point[axis]) - float(actual_point[axis]))
                if delta > box_tolerance:
                    issues.append(f"face {face_index} landmark {point_index} {axis}: delta {delta:.6g} > {box_tolerance}")

        expected_embedding = expected.get("embedding", [])
        actual_embedding = actual.get("embedding", [])
        expected_error = bool(expected.get("embedding_error"))
        actual_error = bool(actual.get("embedding_error"))
        if expected_error != actual_error:
            issues.append(f"face {face_index} embedding error state differs: reference={expected_error} native={actual_error}")
        expected_dimensions = expected.get("embedding_dimensions", len(expected_embedding))
        actual_dimensions = actual.get("embedding_dimensions", len(actual_embedding))
        if expected_dimensions != len(expected_embedding) or actual_dimensions != len(actual_embedding):
            issues.append(f"face {face_index} embedding dimension metadata does not match its vector length")
        if expected_error and actual_error:
            if expected_embedding or actual_embedding or expected_dimensions != 0 or actual_dimensions != 0:
                issues.append(f"face {face_index} errored embeddings must both be empty")
            continue
        vectors_valid = True
        for label, vector, dimensions in (("reference", expected_embedding, expected_dimensions),
                                          ("native", actual_embedding, actual_dimensions)):
            if dimensions != 512 or len(vector) != 512:
                issues.append(f"face {face_index} {label} embedding must contain exactly 512 values (got {len(vector)})")
                vectors_valid = False
                continue
            values = np.asarray(vector, dtype=np.float64)
            norm = float(np.linalg.norm(values))
            if not np.all(np.isfinite(values)):
                issues.append(f"face {face_index} {label} embedding contains non-finite values")
                vectors_valid = False
            elif not np.isfinite(norm) or abs(norm - 1.0) > 1e-3:
                issues.append(f"face {face_index} {label} embedding norm {norm:.8g} is not approximately 1")
                vectors_valid = False
        if vectors_valid:
            lhs = np.asarray(expected_embedding, dtype=np.float64)
            rhs = np.asarray(actual_embedding, dtype=np.float64)
            cosine = float(np.dot(lhs, rhs) / (np.linalg.norm(lhs) * np.linalg.norm(rhs)))
            if not np.isfinite(cosine) or cosine < embedding_cosine:
                issues.append(f"face {face_index} embedding cosine {cosine:.8f} < {embedding_cosine}")
    return issues


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models", type=Path, required=True, help="buffalo_l directory containing the two pinned ONNX files")
    parser.add_argument("--image", type=Path, required=True, help="local input photo")
    parser.add_argument("--reference-source", type=Path,
                        help="directory with the three InsightFace 0.7.3 source files from its sdist")
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--native-example", type=Path, help="optional face_inference_example executable to compare")
    parser.add_argument("--box-tolerance", type=float, default=0.05)
    parser.add_argument("--score-tolerance", type=float, default=1e-4)
    parser.add_argument("--embedding-cosine-min", type=float, default=0.9995)
    parser.add_argument("--warmups", type=int, default=0, help="native example warmup analyses before measurement")
    parser.add_argument("--repeats", type=int, default=1, help="native example measurement repetitions")
    parser.add_argument("--output", type=Path, help="write reference JSON to this path")
    args = parser.parse_args()
    if args.warmups < 0 or args.repeats < 1:
        parser.error("--warmups must be non-negative and --repeats must be at least one")

    result = reference(args.models, args.image, args.device, args.reference_source)
    issues: list[str] = []
    if args.native_example:
        command = [str(args.native_example), "--models", str(args.models), "--image", str(args.image),
                   "--device", args.device, "--warmups", str(args.warmups), "--repeats", str(args.repeats)]
        completed = subprocess.run(command, check=False, capture_output=True, text=True)
        if completed.returncode != 0:
            raise RuntimeError(f"Native example exited {completed.returncode}: {completed.stderr or completed.stdout}")
        native_result = json.loads(completed.stdout)
        issues = compare(result, native_result, args.box_tolerance, args.score_tolerance, args.embedding_cosine_min)
        result["native_comparison"] = {
            "passed": not issues,
            "issues": issues,
            "performance": native_result.get("performance"),
        }

    rendered = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return 1 if issues else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"reference failure: {error}", file=sys.stderr)
        raise SystemExit(2)
