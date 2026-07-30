# Meal Planner

Create a daily three-meal plan from the date, saved preferences, and optional
activity data.

## When to use
When the user asks what to eat, requests a meal plan, or wants a daily calorie
estimate.

## How to use
1. Call `get_current_time` and determine whether today is a weekday.
2. Read `/data/agent/memory/MEMORY.md` for allergies, preferences, and goals.
   Never recommend a known allergen.
3. Optionally call `get_steps` to estimate activity. If unavailable, use a
   neutral activity level.
4. Provide breakfast, lunch, and dinner with approximate calories and a daily
   total. Label all nutrition numbers as estimates.
5. For medical diets, advise confirmation with a qualified clinician.
