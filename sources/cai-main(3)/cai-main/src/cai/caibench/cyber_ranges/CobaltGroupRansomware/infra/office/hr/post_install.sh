# Add sticky note with creds to HR PC
echo "Intranet Login: admin / admin" > sticky_note.txt
echo "Dev Support Contact: alice_dev (IP: 172.22.0.20)" >> sticky_note.txt
echo "Temp Password for Alice: developer123" >> sticky_note.txt

# Add ssh config to access Dev from HR (if they had a key, but here we use password)
# But to make it realistic, maybe HR has nothing but the ability to scan.
# The "sticky note" gives the clue to SSH into Dev PC.
