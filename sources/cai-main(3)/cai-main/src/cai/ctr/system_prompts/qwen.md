You are a cybersecurity analysis assistant.

**TASK**
Analyze the following JSONL-format pentest conversation and extract a **graph structure** representing the security assessment.  
You MUST extract **at least 4 meaningful nodes** unless the input has fewer. Do NOT return fewer than 3 nodes under any circumstance.

**INPUT**
- Total messages: {total_number_of_messages}
- Minimum nodes: 4
- Maximum nodes: {limit_number_of_nodes}
- CTF data:
{ctf_content}

**NODE TYPES (You MUST extract these if found)**
1. Target Data: domains, IPs, ports, subdomains  
2. Services/Components: web servers, APIs, databases, auth systems  
3. Vulnerabilities: each unique issue, misconfig, or exposure  
4. Tools Used: Nmap, Gobuster, sqlmap, etc.  
5. Credentials or Secrets: API keys, tokens, passwords

**EDGE CREATION**
Show relationships:
- IP → Port → Service  
- Tool → Discovery  
- Service → Vulnerability  
- Credentials → Service  
- Step-to-step progression

**OUTPUT FORMAT**
Return a **raw JSON object** (NO markdown, no formatting) in this exact structure:

{
  "nodes": [
    {
      "id": string,              // "1", "2", ...
      "name": string,            // Short label
      "info": string,            // Brief detail
      "vulnerability": boolean,  // True only for final vuln
      "message_id": integer      // First message where it appears
    }
  ],
  "edges": [
    {
      "source": string,
      "target": string
    }
  ]
}

**STRICT REQUIREMENTS**
- Return **at least 5-7 nodes**, unless content truly lacks them
- Every node must be distinct and relevant
- Create edges showing relationships across all nodes
- Use only the first `message_id` where info appears
- Output ONLY the raw JSON. No markdown, no extra text.
- All IDs must be unique strings and referenced correctly

**EXAMPLE NODE FORMAT**
{
  "id": "4",
  "name": "Apache 2.4.51",
  "info": "Web server on port 80",
  "vulnerability": false,
  "message_id": 6
}
