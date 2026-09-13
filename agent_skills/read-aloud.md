# Read Aloud

Read documents or web content aloud via TTS.

## When to use
When user says read this to me, read aloud, or listen to this article.

## How to use
1. Identify the content source:
   - Feishu doc: feishu_doc_read to get content
   - Local file: read_file to get content
   - URL: fetch_url to get web content
2. Clean the text: strip HTML tags, markdown formatting, URLs
3. Split into paragraphs if long (TTS works best with shorter chunks)
4. Respond with the cleaned text — TTS will read it aloud
5. For very long content: summarize first, offer to read sections

## Tip
Keep each response under 200 characters for natural TTS pacing.
For longer content, break into multiple turns.

## Example
User: "帮我朗读记忆文件"
→ read_file /data/ai_agent/memory/MEMORY.md
→ Clean text, split into paragraphs
→ "你的记忆文件内容：用户偏好中文回复，常用嵌入式开发功能..."
(TTS will read the response aloud)
