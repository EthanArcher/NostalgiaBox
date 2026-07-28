import { toFile } from "openai";
import { getOpenAI } from "./openai";

/**
 * Speech-to-text. Transcribes the child's recorded WAV to text.
 * Audio is used transiently here and never persisted.
 *
 * Defaults to whisper-1 (available on every OpenAI account). Falls back across
 * models on error so a single model's availability can't break the box.
 */
export async function transcribe(audio: ArrayBuffer | Buffer): Promise<string> {
  const openai = getOpenAI();
  const buf = Buffer.isBuffer(audio) ? audio : Buffer.from(audio);

  const primary = process.env.STT_MODEL || "whisper-1";
  const models = [primary, "gpt-4o-mini-transcribe", "whisper-1"].filter(
    (m, i, a) => a.indexOf(m) === i,
  );

  let lastErr: unknown;
  for (const model of models) {
    try {
      // Recreate the file each attempt (the upload stream is consumed on use).
      const file = await toFile(buf, "question.wav", { type: "audio/wav" });
      const res = await openai.audio.transcriptions.create({ file, model, response_format: "text" });
      const text = typeof res === "string" ? res : ((res as { text?: string }).text ?? "");
      return text.trim();
    } catch (e) {
      lastErr = e;
      console.error(`[stt] model "${model}" failed:`, (e as Error).message);
    }
  }
  throw lastErr instanceof Error ? lastErr : new Error("transcription failed");
}
