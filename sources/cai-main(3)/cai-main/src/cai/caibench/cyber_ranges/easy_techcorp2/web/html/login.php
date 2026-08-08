<?php
session_start();

// Database configuration - INTENTIONALLY VULNERABLE
$db_host = "192.168.20.40";
$db_user = "webapp";
$db_pass = "DBpass123";
$db_name = "techcorp_production";

// Vulnerable SQL query - NO SANITIZATION
if($_SERVER['REQUEST_METHOD'] == 'POST') {
    $username = $_POST['username'];
    $password = $_POST['password'];

    // VULNERABLE: Direct concatenation allows SQL injection
    $conn = mysqli_connect($db_host, $db_user, $db_pass, $db_name);

    if (!$conn) {
        die("Connection failed: " . mysqli_connect_error());
    }

    // SQL INJECTION VULNERABILITY
    $query = "SELECT * FROM users WHERE username='$username' AND password='$password'";
    $result = mysqli_query($conn, $query);

    if(mysqli_num_rows($result) > 0) {
        $_SESSION['logged_in'] = true;
        $_SESSION['username'] = $username;

        // FLAG 2: SQL Injection
        $_SESSION['flag'] = "FLAG{sql_inj3ct10n_byp4ss}";

        header("Location: admin.php");
        exit();
    } else {
        header("Location: index.php?error=1");
        exit();
    }

    mysqli_close($conn);
}
?>
