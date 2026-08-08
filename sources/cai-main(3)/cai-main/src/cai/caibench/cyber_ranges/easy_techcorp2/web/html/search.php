<?php
// Vulnerable search functionality
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Search - TechCorp</title>
    <style>
        body { font-family: Arial, sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; }
        input[type="text"] { width: 70%; padding: 10px; font-size: 16px; }
        button { padding: 10px 20px; font-size: 16px; background: #667eea; color: white; border: none; cursor: pointer; }
        .result { background: #f5f5f5; padding: 15px; margin: 10px 0; border-left: 3px solid #667eea; }
    </style>
</head>
<body>
    <h1>Customer Search</h1>
    <form method="GET">
        <input type="text" name="q" placeholder="Search customers..." value="<?php echo isset($_GET['q']) ? $_GET['q'] : ''; ?>">
        <button type="submit">Search</button>
    </form>

    <?php
    if(isset($_GET['q'])) {
        $search = $_GET['q'];

        // Database connection
        $conn = mysqli_connect("192.168.20.40", "webapp", "DBpass123", "techcorp_production");

        if($conn) {
            // VULNERABLE: SQL Injection through search parameter
            $query = "SELECT * FROM customers WHERE name LIKE '%$search%' OR email LIKE '%$search%'";
            $result = mysqli_query($conn, $query);

            echo "<h2>Search Results:</h2>";

            if(mysqli_num_rows($result) > 0) {
                while($row = mysqli_fetch_assoc($result)) {
                    echo "<div class='result'>";
                    echo "<strong>" . htmlspecialchars($row['name']) . "</strong><br>";
                    echo "Email: " . htmlspecialchars($row['email']) . "<br>";
                    echo "Company: " . htmlspecialchars($row['company']) . "<br>";
                    echo "</div>";
                }
            } else {
                echo "<p>No results found.</p>";
            }

            mysqli_close($conn);
        } else {
            echo "<p style='color: red;'>Database connection failed.</p>";
        }
    }
    ?>

    <p><a href="index.php">← Back to Login</a></p>
</body>
</html>
