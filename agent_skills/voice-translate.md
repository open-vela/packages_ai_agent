# Voice Translate

Real-time spoken translation via voice channel.

## When to use
When user says translate what I say, interpret for me,
or speaks in one language and wants output in another.

## How to use
1. Voice ASR converts spoken input to text (source language)
2. Identify source and target languages from context or user preference
3. Translate the text using language knowledge
4. For specialized terms: web_search to verify
5. Respond with translation — TTS will speak it in target language
6. Keep translations concise for natural spoken output

## Example
User (voice, Chinese): "这个多少钱？"
Agent (voice, English): "How much is this?"

## Tip
Read /data/ai_agent/config/USER.md for user's preferred languages.
