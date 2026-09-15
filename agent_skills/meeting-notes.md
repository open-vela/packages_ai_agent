# Meeting Notes

Create and save meeting notes to Feishu docs.

## When to use
When user says meeting notes, record meeting, summarize discussion.

## How to use
1. get_current_time for date/time
2. feishu_doc_create with title "Meeting Notes YYYY-MM-DD"
3. Ask user for key points or transcribe from context
4. feishu_doc_write to save content with structure:
   - Date/Time, Attendees, Key Points, Action Items
5. Return the document URL to user

## Important
- You MUST execute tools and respond with results. Do NOT output this template.
- If feishu_doc_create fails (Feishu not configured), tell the user:
  "Feishu is not configured. Run set_feishu_app <app_id> <app_secret> to set up."
  As fallback, offer to save notes locally: write_file /data/ai_agent/memory/daily/YYYY-MM-DD-meeting.md

## Example
User: "create today's meeting notes"
→ get_current_time → 2026-03-28
→ feishu_doc_create {"title": "Meeting Notes 2026-03-28"}
→ "Created Feishu doc. What are the key points?"
